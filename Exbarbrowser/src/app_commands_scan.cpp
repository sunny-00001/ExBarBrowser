// 扫描域命令：目录体积统计（tool.topn / tool.diskusage）与文件摘要（tool.hash）。
//
// 【这一域为什么单独成文件】
// app_commands_files.cpp 已经超了行数上限；而这一域还有**独立的并发约定**：
// 三条命令的活都可能跑很久，于是它们共享同一套约定（工作线程 + 进度 + 取消 +
// async）。把约定收在一个文件里，比散在各处更容易看出它没被破坏。
//
// 【并发约定（照抄，别各自发挥）】
//   1. **准备在 UI 线程、干活在工作线程、收尾在 UI 线程**。
//      ActiveView() 是 COM 调用，只能在 UI 线程；ShowReport 是窗口，也只能在 UI 线程。
//   2. **跨线程只传值**：作业体在工作线程上跑，handler 在 UI 线程上等它，
//      两者之间靠 shared_ptr 共享所有权传报告文本（JSON 结果由作业服务自己保管）。
//   3. **取消是协作式的**：数据层在循环里查取消位。已经被系统调用卡住的那一步
//      中断不了 —— 这是诚实的边界，不要在文档里写成"立即停止"。
//
// 【为什么报告文本在作业体里拼】报告要说的是"扫了多少、停在哪、结论是什么"，
// 这些数字全都来自数据层。拼报告就得用数据层的输出，而数据层在工作线程上跑，
// 所以报告也该在那儿拼好带回来 —— 在 UI 线程上重算一遍等于把活干两遍。
#include "app.h"

#include "app_commands.h"
#include "app_commands_jobs.h"
#include "disk_scan.h"
#include "file_hash.h"
#include "report.h"
#include "shell_ops.h"
#include "view_stats.h"

namespace {

// 等待时限的余量：作业自己的预算跑完之后，还要留一点时间收尾
//（写结果、join 工作线程）。等待时限因此恒大于作业预算，
// 不会出现"作业还在跑就被判超时"。
constexpr unsigned kWaitSlackMs = 5000;

// 体积统计的报告体（人工路径）。
//
// 【为什么以前没有报告】这两条命令过去只有结构化返回值，而命令面板只调
// Invoke 不调 InvokeResult —— 返回值被丢弃，于是人在界面上点了"最大的 N 项"
// 什么都没发生。现在补上报告，长任务跑完不再是"白跑一趟"。
std::wstring BuildScanReport(const diskscan::Result& r) {
    std::wstring head = L"目录：" + r.path + L"\r\n";
    head += L"合计 " + FormatSize(r.totalBytes) + L"，扫描 " + std::to_wstring(r.scannedFiles) +
            L" 个文件 / " + std::to_wstring(r.scannedDirs) + L" 个目录，用时 " +
            std::to_wstring(r.elapsedMs) + L" ms";
    // 先讲完整性再讲结论：把"扫了多少、停在哪"写在数字前面，
    // 否则"目录很干净"会被读成"这个目录就是这么大"。
    if (r.cancelled)
        head += L"（**已取消**：结果不完整）";
    else if (r.timedOut)
        head += L"（**未扫完**：预算用完，结果偏小）";
    else if (r.deniedDirs)
        head += L"（有 " + std::to_wstring(r.deniedDirs) + L" 个目录打不开，结果偏小）";
    else
        head += L"（已扫完）";

    std::wstring body;
    unsigned shown = 0;
    for (const auto& e : r.entries) {
        body += L"\r\n" + std::to_wstring(++shown) + L".  " + FormatSize(e.bytes) + L"  " +
                e.name + (e.isFolder ? L"（目录）" : L"");
        if (e.isFolder && e.items) body += L"  " + std::to_wstring(e.items) + L" 项";
        if (!e.complete) body += L"（子树未扫完）";
    }
    if (body.empty()) body = L"\r\n（没有可统计的内容）";
    return head + body;
}

}  // namespace

void App::RegisterScanCommands() {
    auto regR = cmd_reg::RegisterWithResult;

    // ---- OpTopN / OpDiskUsage：目录体积统计 ----
    //
    // 两条共用 diskscan::Scan，区别只在语义：
    //   OpTopN       —— "这个目录里什么最占空间"，答的是内容本身
    //   OpDiskUsage  —— "这个目录占多少"，答的是总量
    // 之所以分成两条而不是一条带 mode：它们的**主要问题不同**，
    // 调用方关心的是不同的字段（前者看 entries，后者看 totalBytes）。
    // 参照 du（总量）与 ncdu（占比排序）在 Unix 上也是两个工具。
    //
    // 结果里必带 truncated / timedOut / cancelled / elapsedMs 四个"诚实字段"：
    // 只给数字而不说"没算完"，调用方会把不完整值当真实值用。
    //
    // 注意：这里写成两个显式的 regR(CmdId::X, ...) 而不是循环/工厂。
    // 静态检查（check_ai_meta.py 第 3 项）按 `regR(CmdId::名字` 的字面形式
    // 扫描"谁注册了谁"，用变量传 id 就扫不到，会误报"声明了参数却没取用"。
    // 换句话说：字面注册让检查能覆盖到，这比少写两行代码更重要。
    auto scanImpl = [this](const Args& args, json::Value* result, const wchar_t* kind,
                           const wchar_t* title, bool human) {
        if (!result) return false;
        diskscan::Options opt;
        if (args.Has(L"limit")) opt.limit = args.Int(L"limit", 50);
        const bool async = args.Bool(L"async", false);
        // 预算的默认值随"谁在等"而变（理由见 appjobs::ResolveBudget）。
        const unsigned long long budgetMs = appjobs::ResolveBudget(
            args.Has(L"budgetMs"), args.Int(L"budgetMs", 1500), async, human);
        opt.budgetMs = budgetMs;
        opt.noTimeLimit = scanctl::IsUnlimited(budgetMs);

        // 没指定路径就统计当前目录 —— 这是最常见的用法（"这里什么最占空间"）。
        // 注意 ActiveView() 是 AddRef 过的，必须 Release，否则每调一次漏一个引用。
        std::wstring target = args.Str(L"path");
        if (target.empty()) {
            if (IShellView* v = ActiveView()) {
                target = ViewFolderPath(v);
                v->Release();
            }
        }
        if (target.empty()) {
            LogDbg(L"[cmd] 扫描命令：拿不到当前目录");
            CommandBus::Get().NoteFailure(L"拿不到当前目录");
            return false;
        }

        // 报告文本要跨线程带出来：作业体在工作线程上跑，handler 在 UI 线程上等它。
        auto report = std::make_shared<std::wstring>();

        // 作业体：重活在工作线程上跑。
        // o.ctl 挂上取消位与进度回调，数据层就会在遍历中查取消位并回报进度。
        job::Body body = [opt, target, report](scanctl::Control& ctl, json::Value* out,
                                               std::wstring* err) -> bool {
            diskscan::Options o = opt;
            o.ctl = &ctl;
            diskscan::Result r;
            if (!diskscan::Scan(target, o, &r, err)) return false;

            out->Set(L"path", json::Value::MakeStr(r.path));
            out->Set(L"totalBytes", json::Value::MakeNum(static_cast<double>(r.totalBytes)));
            out->Set(L"truncated", json::Value::MakeBool(r.truncated));
            out->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
            out->Set(L"cancelled", json::Value::MakeBool(r.cancelled));
            out->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));
            out->Set(L"scannedFiles", json::Value::MakeNum(static_cast<double>(r.scannedFiles)));
            out->Set(L"scannedDirs", json::Value::MakeNum(static_cast<double>(r.scannedDirs)));
            out->Set(L"deniedDirs", json::Value::MakeNum(static_cast<double>(r.deniedDirs)));
            out->Set(L"complete", json::Value::MakeBool(r.complete));

            json::Value arr = json::Value::MakeArray();
            for (const auto& e : r.entries) {
                json::Value o = json::Value::MakeObject();
                o.Set(L"name", json::Value::MakeStr(e.name));
                o.Set(L"path", json::Value::MakeStr(e.path));
                o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(e.bytes)));
                o.Set(L"isFolder", json::Value::MakeBool(e.isFolder));
                o.Set(L"items", json::Value::MakeNum(static_cast<double>(e.items)));
                o.Set(L"complete", json::Value::MakeBool(e.complete));
                arr.arr.push_back(std::move(o));
            }
            out->Set(L"entries", arr);
            *report = BuildScanReport(r);
            return true;
        };

        appjobs::TaskSpec spec;
        spec.kind = kind;
        spec.title = title;
        spec.target = target;
        spec.async = async;
        spec.human = human;
        // 不限时（0）没有"等多久"可言；机器同步路径的预算恒为正数（见 ResolveBudget）。
        spec.waitMs = budgetMs ? static_cast<unsigned>(budgetMs + kWaitSlackMs) : 0;

        const std::wstring id = appjobs::RunTask(hwnd_, theme_, spec, std::move(body));
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"无法启动后台作业");
            return false;
        }
        std::wstring err;
        FailKind failKind = FailKind::PreconditionUnmet;
        if (!appjobs::CollectResult(id, async, result, &err, &failKind)) {
            // 类别由作业层给（取消 / 超时 / 真失败），这里只负责转交 ——
            // 调用方拿到的是能指导下一步的机器码，不是一句要猜的中文。
            CommandBus::Get().NoteFailure(err, failKind);
            return false;
        }
        // 人工路径：跑完给一份可读报告（命令面板只调 Invoke，不看返回值）。
        if (human && !async) ShowReport(hwnd_, theme_, title, *report);
        return true;
    };
    regR(CmdId::OpTopN, [scanImpl](const Args& a, json::Value* r) {
        return scanImpl(a, r, L"topn", L"最大的 N 项", IsHumanCall(a));
    });
    regR(CmdId::OpDiskUsage, [scanImpl](const Args& a, json::Value* r) {
        return scanImpl(a, r, L"usage", L"目录占用", IsHumanCall(a));
    });

    // ---- FileHash：文件摘要 ----
    //
    // 人工路径 —— **不带参数**调用（命令面板 / 热键）：目标取界面现状
    //             （哈希取选中项），结果用 ShowReport 弹只读文本报告；
    // agent 路径 —— 带参数调用：返回结构化 JSON，**不弹窗**（没人看着屏幕）。
    // 判据是 IsHumanCall(args)（空参数 **且** 不是机器调用），不是 args.IsEmpty()：
    // 无参数命令从 AI 过来也是 {}，只看空参数会走"人侧"分支弹模态窗口，
    // 把管道调用卡到超时。命令面板与热键走的正是空参数，所以人侧判定要成立。
    // 副作用一致（都只读），所以不需要两套命令 id。
    regR(CmdId::FileHash, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = IsHumanCall(args);

        filehash::Algo algo = filehash::Algo::Sha256;
        if (args.Has(L"algorithm") && !filehash::ParseAlgo(args.Str(L"algorithm"), &algo)) {
            // 认不出的算法：失败，不静默退回 SHA-256。否则"我算的是 SHA-512"
            // 与实际结果无法区分 —— 拿错算法去核对下载包会得出错误结论。
            LogDbg(L"[cmd] tool.hash：认不出的算法 %s", args.Str(L"algorithm").c_str());
            // 原因要带给调用方：名字打错和"值超范围"该做的下一步完全不同。
            CommandBus::Get().NoteFailure(L"不支持的算法：" + args.Str(L"algorithm") +
                                          L"（可用 md5 / sha1 / sha256）");
            return false;
        }

        std::vector<std::wstring> paths = args.StrArray(L"paths");
        if (paths.empty()) {
            // 无参：当前选中项。这是人最自然的动作（选中 → 算摘要）。
            if (IShellView* v = ActiveView()) {
                paths = shellops::SelectedPaths(v);
                v->Release();
            }
        }
        if (paths.empty()) {
            CommandBus::Get().NoteFailure(L"没有目标：paths 为空，且当前没有选中项");
            if (human)
                MessageBoxW(hwnd_, L"先在文件列表里选中要计算摘要的文件。", id::kBrandShort,
                            MB_ICONINFORMATION);
            return false;
        }

        const bool async = args.Bool(L"async", false);
        const unsigned long long budgetMs = appjobs::ResolveBudget(
            args.Has(L"budgetMs"), args.Int(L"budgetMs", 1500), async, human);
        auto report = std::make_shared<std::wstring>();

        job::Body body = [algo, paths, budgetMs, report](scanctl::Control& ctl, json::Value* out,
                                                          std::wstring* err) -> bool {
            json::Value arr = json::Value::MakeArray();
            std::wstring text;
            std::wstring failures;
            unsigned okCount = 0, failCount = 0;
            bool timedOut = false;
            bool cancelled = false;
            // 时间预算：**整次调用一份**，不是每个文件一份。
            // 否则 N 个文件就是 N 倍预算 —— 而"每个文件都算完"并不是调用方要的。
            // budgetMs==0 → 不限（异步与人工路径的默认值；见 appjobs::ResolveBudget）。
            const ULONGLONG deadline = budgetMs > 0 ? GetTickCount64() + budgetMs : 0;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (ctl.Cancelled()) {
                    cancelled = true;
                    break;
                }
                const std::wstring& p = paths[i];
                // 进度报**两次**：动手前报"在算第几个"（界面立刻有反应），
                // 算完再报一次把"已处理数"推上去 ——
                // 只报动手前的那次，"已处理数"永远落后一个，
                // 看起来就像"算完了却一个都没算"。
                ctl.Report(L"正在计算摘要", static_cast<unsigned long long>(i),
                           static_cast<unsigned long long>(paths.size()));
                const ULONGLONG now = GetTickCount64();
                if (deadline && now >= deadline) {
                    // 预算已经用完：剩下的文件**如实说没算**，不假装算过。
                    timedOut = true;
                    json::Value o = json::Value::MakeObject();
                    o.Set(L"path", json::Value::MakeStr(p));
                    o.Set(L"algorithm", json::Value::MakeStr(filehash::AlgoName(algo)));
                    o.Set(L"ok", json::Value::MakeBool(false));
                    o.Set(L"error", json::Value::MakeStr(L"超出时间预算，未计算"));
                    arr.arr.push_back(std::move(o));
                    ++failCount;
                    failures += L"  " + p + L"  —  超出时间预算，未计算\r\n";
                    continue;
                }
                filehash::Result hr;
                // 剩余预算传 0 = 不限（HashFile 认 0 为不限，见 file_hash.h）。
                // deadline==0 时没有"剩余"可言，所以也传 0。
                const unsigned long long remain = deadline ? (deadline - now) : 0;
                const bool ok = filehash::HashFile(p, algo, &hr, ctl.cancel, remain);
                if (hr.timedOut) timedOut = true;
                if (hr.cancelled) cancelled = true;
                if (ok) {
                    ++okCount;
                    text += hr.hex + L"  " + FormatSize(hr.bytes) + L"  " + p + L"\r\n";
                } else {
                    ++failCount;
                    failures += L"  " + p + L"  —  " + hr.error + L"\r\n";
                }
                json::Value o = json::Value::MakeObject();
                o.Set(L"path", json::Value::MakeStr(p));
                o.Set(L"algorithm", json::Value::MakeStr(filehash::AlgoName(algo)));
                o.Set(L"ok", json::Value::MakeBool(ok));
                if (ok) {
                    o.Set(L"hash", json::Value::MakeStr(hr.hex));
                    o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(hr.bytes)));
                } else {
                    o.Set(L"error", json::Value::MakeStr(hr.error));
                }
                arr.arr.push_back(std::move(o));
                // 把"已处理数"推上去（见上面为什么报两次）。
                ctl.Report(L"正在计算摘要", static_cast<unsigned long long>(i + 1),
                           static_cast<unsigned long long>(paths.size()));
                if (cancelled) break;
            }

            out->Set(L"algorithm", json::Value::MakeStr(filehash::AlgoName(algo)));
            out->Set(L"entries", arr);
            // 完整性必须说清楚：只给一串哈希而不说"有没有算完"，
            // 调用方会把"预算用完"当成"这些文件就是这些摘要"（其实可能算的是半个文件）。
            out->Set(L"timedOut", json::Value::MakeBool(timedOut));
            out->Set(L"cancelled", json::Value::MakeBool(cancelled));
            out->Set(L"complete", json::Value::MakeBool(!timedOut && !cancelled));

            std::wstring head = std::wstring(filehash::AlgoName(algo)) + L"　成功 " +
                                std::to_wstring(okCount) + L" 个";
            if (failCount) head += L"，失败 " + std::to_wstring(failCount) + L" 个";
            if (cancelled) head += L"（**已取消**：结果不完整）";
            else if (timedOut) head += L"（**未算完**：超出时间预算，结果不完整）";
            head += L"\r\n\r\n" + text;
            if (failCount) head += L"\r\n读取失败：\r\n" + failures;
            *report = head;
            return true;
        };

        appjobs::TaskSpec spec;
        spec.kind = L"hash";
        spec.title = L"文件摘要";
        spec.target = paths.size() == 1 ? paths[0] : (std::to_wstring(paths.size()) + L" 个文件");
        spec.async = async;
        spec.human = human;
        // 不限时（0）没有"等多久"可言；机器同步路径的预算恒为正数（见 ResolveBudget）。
        spec.waitMs = budgetMs ? static_cast<unsigned>(budgetMs + kWaitSlackMs) : 0;

        const std::wstring id = appjobs::RunTask(hwnd_, theme_, spec, std::move(body));
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"无法启动后台作业");
            return false;
        }
        std::wstring err;
        FailKind failKind = FailKind::PreconditionUnmet;
        if (!appjobs::CollectResult(id, async, result, &err, &failKind)) {
            // 类别由作业层给（取消 / 超时 / 真失败），这里只负责转交 ——
            // 调用方拿到的是能指导下一步的机器码，不是一句要猜的中文。
            CommandBus::Get().NoteFailure(err, failKind);
            return false;
        }
        if (human && !async) ShowReport(hwnd_, theme_, L"文件摘要", *report);
        return true;
    });
}
