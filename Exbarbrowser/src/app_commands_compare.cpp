// 比较域命令：重复文件查找（tool.dupfinder）与目录比较（tool.compare）。
//
// 【这一域为什么单独成文件】
// app_commands_files.cpp 已经超了行数上限；而这一域有**独立的并发约定**：
// 两条命令都要遍历目录树，天然可能跑很久，于是共享同一套约定
//（工作线程 + 进度 + 取消 + async）。把约定收在一个文件里，
// 比散在各处更容易看出它没被破坏。
//
// 【并发约定】与 app_commands_scan.cpp 一致，不要各自发挥：
//   1. 准备在 UI 线程、干活在工作线程、收尾在 UI 线程；
//   2. 跨线程只传值（报告文本用 shared_ptr 带出来）；
//   3. 取消是协作式的（数据层在循环里查取消位）。
#include "app.h"

#include "app_commands.h"
#include "app_commands_jobs.h"
#include "file_tools.h"
#include "prompt.h"
#include "report.h"
#include "shell_ops.h"
#include "view_stats.h"

namespace {

// 等待时限的余量：作业自己的预算跑完之后，还要留一点时间收尾。
constexpr unsigned kWaitSlackMs = 5000;

// 重复文件的报告体（人工路径）。
//
// 【为什么先讲完整性再讲结论】把"扫了多少、停在哪"写在数字前面，
// 否则"没有重复"会被读成"这个目录很干净"，而实际可能是预算用完了。
std::wstring BuildDupReport(const filetools::DupResult& r) {
    std::wstring head = L"目录：" + r.root + L"\r\n";
    head += L"扫描 " + std::to_wstring(r.scannedFiles) + L" 个文件，实际哈希 " +
            std::to_wstring(r.hashedFiles) + L" 个，用时 " + std::to_wstring(r.elapsedMs) + L" ms";
    if (r.cancelled)
        head += L"（**已取消**：结果不完整）";
    else if (r.timedOut)
        head += L"（**未扫完**：预算用完，结果偏少）";
    else
        head += L"（已扫完）";
    if (r.deniedDirs)
        head += L"\r\n有 " + std::to_wstring(r.deniedDirs) + L" 个目录打不开（结果偏少）";
    head += L"\r\n可回收 " + FormatSize(r.wastedBytes) + L"，共 " +
            std::to_wstring(r.groups.size()) + L" 组\r\n";

    std::wstring body;
    constexpr size_t kMaxShown = 200;
    size_t shown = 0;
    for (const auto& g : r.groups) {
        if (shown < kMaxShown) {
            const unsigned long long wasted = g.bytes * (g.files.size() - 1);
            body += L"\r\n组 " + std::to_wstring(shown + 1) + L"：" +
                    std::to_wstring(g.files.size()) + L" 份 × " + FormatSize(g.bytes) +
                    L" = 可回收 " + FormatSize(wasted) + L"\r\n  SHA-256 " + g.hash + L"\r\n";
            for (const auto& f : g.files) body += L"  " + f.path + L"\r\n";
            ++shown;
        }
    }
    if (r.groups.size() > kMaxShown)
        body += L"\r\n（仅显示前 " + std::to_wstring(kMaxShown) + L" 组）\r\n";
    return head + body;
}

// 目录比较的报告体（人工路径）。
std::wstring BuildCompareReport(const filetools::CompareResult& r, bool byContent) {
    std::wstring head = L"左：" + r.left + L"\r\n右：" + r.right + L"\r\n";
    head += L"相同 " + std::to_wstring(r.sameCount) + L"，内容不同 " +
            std::to_wstring(r.differCount) + L"，仅左 " + std::to_wstring(r.onlyLeftCount) +
            L"，仅右 " + std::to_wstring(r.onlyRightCount) + L"，用时 " +
            std::to_wstring(r.elapsedMs) + L" ms";
    if (r.cancelled)
        head += L"（**已取消**：结果不完整）";
    else if (r.timedOut)
        head += L"（**未比完**：预算用完）";
    else
        head += L"（已比完）";
    if (r.deniedLeft || r.deniedRight)
        head += L"\r\n有目录打不开：左 " + std::to_wstring(r.deniedLeft) + L"，右 " +
                std::to_wstring(r.deniedRight) + L"（结果偏少）";
    head += L"\r\n左合计 " + FormatSize(r.leftBytes) + L"，右合计 " + FormatSize(r.rightBytes) +
            L"\r\n";
    if (!byContent) head += L"\r\n（只比了大小，同大小不同内容会算作“相同”）\r\n";

    std::wstring body;
    for (const auto& e : r.entries) {
        const wchar_t* tag = e.state == filetools::Diff::OnlyLeft    ? L"[L] 仅左侧"
                             : e.state == filetools::Diff::OnlyRight ? L"[R] 仅右侧"
                                                              : L"[M] 内容不同";
        body += std::wstring(tag) + L"  " + FormatSize(e.leftBytes) + L" → " +
                FormatSize(e.rightBytes) + L"  " + e.rel + L"\r\n";
    }
    return head + L"\r\n" + body;
}

}  // namespace

void App::RegisterCompareCommands() {
    auto regR = cmd_reg::RegisterWithResult;

    // ---- DupFinder：查找重复文件 ----
    regR(CmdId::DupFinder, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = IsHumanCall(args);

        filetools::Options opt;
        const bool async = args.Bool(L"async", false);
        // 预算的默认值随"谁在等"而变（理由见 appjobs::ResolveBudget）。
        const unsigned long long budgetMs = appjobs::ResolveBudget(
            args.Has(L"budgetMs"), args.Int(L"budgetMs", 1500), async, human);
        opt.budgetMs = budgetMs;
        opt.noTimeLimit = scanctl::IsUnlimited(budgetMs);
        if (args.Has(L"minBytes")) {
            const int n = args.Int(L"minBytes", 1);
            opt.minBytes = n <= 0 ? 0ULL : static_cast<unsigned long long>(n);
        }

        std::wstring root = args.Str(L"path");
        if (root.empty()) {
            if (IShellView* v = ActiveView()) {
                root = ViewFolderPath(v);
                v->Release();
            }
        }
        if (root.empty()) {
            CommandBus::Get().NoteFailure(L"没有目录：path 为空，且当前视图没有文件系统路径");
            if (human)
                MessageBoxW(hwnd_, L"拿不到当前目录（虚拟位置没有文件系统路径）。",
                            id::kBrandShort, MB_ICONINFORMATION);
            return false;
        }

        auto report = std::make_shared<std::wstring>();

        job::Body body = [opt, root, report](scanctl::Control& ctl, json::Value* out,
                                             std::wstring* err) -> bool {
            filetools::Options o = opt;
            o.ctl = &ctl;
            filetools::DupResult r;
            if (!filetools::FindDuplicates(root, o, &r, err)) return false;

            json::Value groups = json::Value::MakeArray();
            for (const auto& g : r.groups) {
                json::Value go = json::Value::MakeObject();
                go.Set(L"bytes", json::Value::MakeNum(static_cast<double>(g.bytes)));
                go.Set(L"hash", json::Value::MakeStr(g.hash));
                json::Value files = json::Value::MakeArray();
                for (const auto& f : g.files) {
                    json::Value fo = json::Value::MakeObject();
                    fo.Set(L"path", json::Value::MakeStr(f.path));
                    fo.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
                    files.arr.push_back(std::move(fo));
                }
                go.Set(L"files", files);
                groups.arr.push_back(std::move(go));
            }

            out->Set(L"path", json::Value::MakeStr(r.root));
            out->Set(L"groups", groups);
            out->Set(L"wastedBytes", json::Value::MakeNum(static_cast<double>(r.wastedBytes)));
            out->Set(L"scannedFiles", json::Value::MakeNum(static_cast<double>(r.scannedFiles)));
            out->Set(L"hashedFiles", json::Value::MakeNum(static_cast<double>(r.hashedFiles)));
            out->Set(L"deniedDirs", json::Value::MakeNum(static_cast<double>(r.deniedDirs)));
            out->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
            out->Set(L"cancelled", json::Value::MakeBool(r.cancelled));
            out->Set(L"complete", json::Value::MakeBool(r.complete));
            out->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));
            *report = BuildDupReport(r);
            return true;
        };

        appjobs::TaskSpec spec;
        spec.kind = L"dup";
        spec.title = L"查找重复文件";
        spec.target = root;
        spec.async = async;
        spec.human = human;
        // 不限时（0）没有"等多久"可言；机器同步路径的预算恒为正数（见 ResolveBudget）。
        spec.waitMs = budgetMs ? static_cast<unsigned>(budgetMs + kWaitSlackMs) : 0;

        const std::wstring id = appjobs::RunTask(hwnd_, theme_, spec, std::move(body));
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"无法启动后台作业", FailKind::NotSupported);
            return false;
        }
        std::wstring err;
        FailKind kind = FailKind::PreconditionUnmet;
        if (!appjobs::CollectResult(id, async, result, &err, &kind)) {
            // 类别由作业层给（取消 / 超时 / 真失败），这里只负责转交 ——
            // 调用方拿到的是能指导下一步的机器码，不是一句要猜的中文。
            CommandBus::Get().NoteFailure(err, kind);
            return false;
        }
        if (human && !async) ShowReport(hwnd_, theme_, L"查找重复文件", *report);
        return true;
    });

    // ---- FolderCompare：比较两个文件夹 ----
    regR(CmdId::FolderCompare, [this](const Args& args, json::Value* result) {
        if (!result) return false;
        const bool human = IsHumanCall(args);

        std::wstring left = args.Str(L"left");
        std::wstring right = args.Str(L"right");
        if ((left.empty() || right.empty()) && human) {
            // 人没有"两个参数"这回事：给一个两框输入，左侧预填当前目录。
            if (left.empty()) {
                if (IShellView* v = ActiveView()) {
                    left = ViewFolderPath(v);
                    v->Release();
                }
            }
            if (!PromptText2(hwnd_, theme_, L"比较文件夹", L"左侧（当前目录）", left, &left,
                             L"右侧（要对比的目录）", right, &right))
                return false;
        }
        if (left.empty() || right.empty()) {
            CommandBus::Get().NoteFailure(L"两个目录都必须给：left / right");
            return false;
        }

        filetools::CompareOptions opt;
        if (args.Has(L"byContent")) opt.byContent = args.Bool(L"byContent", true);
        const bool async = args.Bool(L"async", false);
        // 预算的默认值随"谁在等"而变（理由见 appjobs::ResolveBudget）。
        const unsigned long long budgetMs = appjobs::ResolveBudget(
            args.Has(L"budgetMs"), args.Int(L"budgetMs", 1500), async, human);
        opt.walk.budgetMs = budgetMs;
        opt.walk.noTimeLimit = scanctl::IsUnlimited(budgetMs);
        auto report = std::make_shared<std::wstring>();

        job::Body body = [opt, left, right, report](scanctl::Control& ctl, json::Value* out,
                                                    std::wstring* err) -> bool {
            filetools::CompareOptions o = opt;
            o.walk.ctl = &ctl;
            filetools::CompareResult r;
            if (!filetools::CompareFolders(left, right, o, &r, err)) return false;

            json::Value entries = json::Value::MakeArray();
            for (const auto& e : r.entries) {
                json::Value ov = json::Value::MakeObject();
                ov.Set(L"rel", json::Value::MakeStr(e.rel));
                ov.Set(L"leftBytes", json::Value::MakeNum(static_cast<double>(e.leftBytes)));
                ov.Set(L"rightBytes", json::Value::MakeNum(static_cast<double>(e.rightBytes)));
                ov.Set(L"state", json::Value::MakeStr(e.state == filetools::Diff::OnlyLeft
                                                          ? L"onlyLeft"
                                                      : e.state == filetools::Diff::OnlyRight
                                                          ? L"onlyRight"
                                                          : L"differ"));
                entries.arr.push_back(std::move(ov));
            }

            out->Set(L"left", json::Value::MakeStr(r.left));
            out->Set(L"right", json::Value::MakeStr(r.right));
            out->Set(L"entries", entries);
            out->Set(L"sameCount", json::Value::MakeNum(static_cast<double>(r.sameCount)));
            out->Set(L"differCount", json::Value::MakeNum(static_cast<double>(r.differCount)));
            out->Set(L"onlyLeftCount", json::Value::MakeNum(static_cast<double>(r.onlyLeftCount)));
            out->Set(L"onlyRightCount",
                     json::Value::MakeNum(static_cast<double>(r.onlyRightCount)));
            out->Set(L"timedOut", json::Value::MakeBool(r.timedOut));
            out->Set(L"cancelled", json::Value::MakeBool(r.cancelled));
            out->Set(L"complete", json::Value::MakeBool(r.complete));
            out->Set(L"elapsedMs", json::Value::MakeNum(static_cast<double>(r.elapsedMs)));
            *report = BuildCompareReport(r, o.byContent);
            return true;
        };

        appjobs::TaskSpec spec;
        spec.kind = L"compare";
        spec.title = L"比较文件夹";
        spec.target = left + L"  ↔  " + right;
        spec.async = async;
        spec.human = human;
        // 不限时（0）没有"等多久"可言；机器同步路径的预算恒为正数（见 ResolveBudget）。
        spec.waitMs = budgetMs ? static_cast<unsigned>(budgetMs + kWaitSlackMs) : 0;

        const std::wstring id = appjobs::RunTask(hwnd_, theme_, spec, std::move(body));
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"无法启动后台作业", FailKind::NotSupported);
            return false;
        }
        std::wstring err;
        FailKind kind = FailKind::PreconditionUnmet;
        if (!appjobs::CollectResult(id, async, result, &err, &kind)) {
            // 类别由作业层给（取消 / 超时 / 真失败），这里只负责转交 ——
            // 调用方拿到的是能指导下一步的机器码，不是一句要猜的中文。
            CommandBus::Get().NoteFailure(err, kind);
            return false;
        }
        if (human && !async) ShowReport(hwnd_, theme_, L"比较文件夹", *report);
        return true;
    });
}
