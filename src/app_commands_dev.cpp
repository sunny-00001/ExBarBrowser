// 开发面命令的处理器（命令表与接口见 commands.h / dev_tools.h）。
//
// 只做三件事：取参 → 调 devtools → 把结构化结果交给命令总线。
// 真正的进程管理/回滚校验在 dev_tools.cpp，命令行与 UI 都走那一份。
//
// 【开发面的三条硬约束】（与 docs/GAP-ANALYSIS-EXTERNAL.md §3.1 对应）
//   1. **参数不进命令行**：target / filter 都先过白名单，再作为**独立参数**
//      传给 CreateProcess，不存在字符串拼接，也就没有注入面。
//   2. **评估器不可自改**：构建器与测试脚本是评估面。跑之前实测一次完整性，
//      被改过就拒绝启动 —— "测试通过"在评估器被改软时是没有意义的。
//   3. **危险动作走双门**：dev.rollback 会覆盖工作树内容，声明 destructive=1，
//      由 ai_api 的双门（设置开关 + approve）把关；
//      并且**必须带 snapshotId** —— 双门只防"未授权"，防不了"授权后不可逆"，
//      所以"有没有退路"要在命令层就挡住（见 src/dev_snapshot.h）。
#include "app.h"

#include <string>
#include <vector>

#include "app_commands.h"
#include "dev_snapshot.h"
#include "dev_tools.h"
#include "eval_surface.h"
#include "str.h"

namespace {

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

// 测试过滤器：只允许"名字里会出现的字符"。
// tests/run_all.sh 拿它做子串匹配，测试名由字母数字下划线构成 ——
// 放行别的字符没有意义，只会变成注入面。
bool ValidTestFilter(const std::wstring& s, std::wstring* err) {
    if (s.size() > 64) {
        if (err) *err = L"过滤器太长（最多 64 字符）";
        return false;
    }
    for (const wchar_t c : s) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'.';
        if (!ok) {
            if (err) {
                *err = L"过滤器只能含字母、数字、下划线、连字符与点（当前含非法字符）";
            }
            return false;
        }
    }
    return true;
}

// 评估面完整性检查。返回 false 表示**不允许继续**，err 给原因。
bool RequireIntactEvalSurface(std::wstring* err) {
    const evalsurface::Report ev = evalsurface::Check();
    if (ev.ok) return true;
    if (err) *err = ev.reason;
    return false;
}

}  // namespace

void App::RegisterDevCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    regR(CmdId::DevSnapshot, [](const Args&, json::Value* out) {
        json::Value snapshot;
        std::wstring err;
        if (!json::Parse(devtools::Snapshot(), snapshot, &err) || !snapshot.IsObject()) {
            CommandBus::Get().NoteFailure(L"快照解析失败：" + err);
            return false;
        }
        // **落盘**一份快照（存内容，不是摘要）—— 见 src/dev_snapshot.h。
        // 没有这一步，dev.rollback 丢掉的改动就再也找不回来
        // （P0-5：旧版"先 snapshot 再 rollback"的保护是空的）。
        const devsnap::Snapshot snap = devsnap::Create();
        if (!snap.ok) {
            CommandBus::Get().NoteFailure(
                snap.error.empty() ? L"快照落盘失败" : snap.error);
            return false;
        }
        snapshot.Set(L"snapshotId", Str(snap.id));
        snapshot.Set(L"snapshotDir", Str(snap.dir));
        snapshot.Set(L"createdAt", Str(snap.createdAt));
        int saved = 0;
        json::Value files = json::Value::MakeArray();
        // changedFiles：P0-9 的"改写可见性"观测面 ——
        // 路径 + mtime + SHA-256。调用方拿它能**独立核对**"现在这份是不是
        // 我上次看到的那一份"，不必相信 git status 的一段文本；
        // 两次快照之间比 sha256，就知道内容有没有被改过。
        json::Value changed = json::Value::MakeArray();
        for (const auto& f : snap.files) {
            if (f.saved) ++saved;
            json::Value o = json::Value::MakeObject();
            o.Set(L"path", Str(f.path));
            o.Set(L"saved", json::Value::MakeBool(f.saved));
            o.Set(L"tracked", json::Value::MakeBool(f.tracked));
            // 用 MakeNum 而不是 MakeInt：bytes 是 64 位，int 会截断到 31 位 ——
            // 一个 3 GB 的文件会报成 2 GB（看起来像"文件变小了"）。
            o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
            // P0-9：快照也要带这两个字段，agent 才能核对"落盘的是哪一版"。
            // mtime 是十进制 Unix 秒的字符串（避开 32 位溢出），空串=取不到。
            o.Set(L"mtime", Str(f.mtime));
            o.Set(L"sha256", Str(f.sha256));
            if (!f.reason.empty()) o.Set(L"reason", Str(f.reason));
            files.arr.push_back(std::move(o));

            json::Value c = json::Value::MakeObject();
            c.Set(L"path", Str(f.path));
            c.Set(L"tracked", json::Value::MakeBool(f.tracked));
            c.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
            c.Set(L"mtime", Str(f.mtime));
            c.Set(L"sha256", Str(f.sha256));
            changed.arr.push_back(std::move(c));
        }
        snapshot.Set(L"savedFiles", json::Value::MakeInt(saved));
        snapshot.Set(L"totalFiles", json::Value::MakeInt(static_cast<int>(snap.files.size())));
        snapshot.Set(L"files", std::move(files));
        snapshot.Set(L"changedFiles", std::move(changed));
        *out = std::move(snapshot);
        return true;
    });

    regR(CmdId::DevDiff, [](const Args& args, json::Value* out) {
        // dev.diff 是纯只读观测：不落盘、不改工作树、不建快照。
        // 审阅"agent 改了什么"不该有副作用 —— 那是 dev.snapshot 的职责。
        //
        // path 可选：只审一个文件时用它，能把一次调用压到 1 条 git 命令；
        // 省略则给全部改动文件（超预算会显式标 truncated，不会假装给全了）。
        const std::wstring path = str::Trim(args.Str(L"path"));
        if (!path.empty()) {
            // 路径必须是仓库内相对路径：这条命令会照着它去读文件内容。
            std::wstring err;
            if (!devtools::ValidRepoPath(path, &err)) {
                CommandBus::Get().NoteFailure(err.empty() ? L"路径不合法" : err);
                return false;
            }
        }
        json::Value result;
        std::wstring err;
        if (!devtools::Diff(path, &result, &err)) {
            // git 不可用要如实报错，不返回一个"看起来没改动"的空 diff（那是假绿）。
            CommandBus::Get().NoteFailure(err.empty() ? L"无法读取工作树 diff" : err);
            return false;
        }
        *out = std::move(result);
        return true;
    });

    regR(CmdId::DevBuild, [](const Args& args, json::Value* out) {
        // target 是**枚举**，不是自由文本：只有 all / clean 两种。
        // 以前这里是 `command += L" " + target`，一个 `&& calc` 就是任意执行。
        const std::wstring target = str::Trim(args.Str(L"target"));
        std::vector<std::wstring> argv;
        argv.push_back(L"build_fast.py");
        if (target == L"clean") {
            argv.push_back(L"--clean");
        } else if (!target.empty() && target != L"all") {
            CommandBus::Get().NoteFailure(L"target 只能是 all（默认）或 clean");
            return false;
        }

        std::wstring err;
        if (!RequireIntactEvalSurface(&err)) {
            CommandBus::Get().NoteFailure(err);
            return false;
        }

        // 硬超时是**常量**（devtools::kBuildTimeoutMs）：不让 agent 自己填时限 ——
        // 让被看管的人决定看管多久，等于没有闸门（理由见 dev_tools.h）。
        const std::wstring id =
            devtools::StartJob(L"build", L"python", argv, devtools::kBuildTimeoutMs);
        out->Set(L"jobId", Str(id));
        out->Set(L"command", Str(target == L"clean" ? L"python build_fast.py --clean"
                                                    : L"python build_fast.py"));
        out->Set(L"timeoutMs", json::Value::MakeInt(
                                   static_cast<int>(devtools::kBuildTimeoutMs)));
        return true;
    });

    regR(CmdId::DevTest, [](const Args& args, json::Value* out) {
        const std::wstring filter = str::Trim(args.Str(L"filter"));
        std::wstring err;
        if (!ValidTestFilter(filter, &err)) {
            CommandBus::Get().NoteFailure(err);
            return false;
        }
        if (!RequireIntactEvalSurface(&err)) {
            CommandBus::Get().NoteFailure(err);
            return false;
        }
        std::vector<std::wstring> argv;
        argv.push_back(L"tests/run_all.sh");
        if (!filter.empty()) argv.push_back(filter);
        const std::wstring id =
            devtools::StartJob(L"test", L"bash", argv, devtools::kTestTimeoutMs);
        out->Set(L"jobId", Str(id));
        out->Set(L"command", Str(filter.empty() ? L"bash tests/run_all.sh"
                                                : L"bash tests/run_all.sh " + filter));
        out->Set(L"timeoutMs", json::Value::MakeInt(
                                   static_cast<int>(devtools::kTestTimeoutMs)));
        return true;
    });

    regR(CmdId::DevJob, [](const Args& args, json::Value* out) {
        const std::wstring id = args.Str(L"id");
        devtools::Job job;
        if (id.empty() || !devtools::GetJob(id, &job)) {
            CommandBus::Get().NoteFailure(L"没有这个作业 id：" + id);
            return false;
        }
        out->Set(L"id", Str(job.id));
        out->Set(L"kind", Str(job.kind));
        out->Set(L"command", Str(job.command));
        out->Set(L"done", json::Value::MakeBool(job.done));
        out->Set(L"exitCode", json::Value::MakeInt(job.exitCode));
        out->Set(L"output", Str(job.output));
        out->Set(L"truncated", json::Value::MakeBool(job.truncated));
        // timedOut 与 exitCode 要分开看：exitCode != 0 是"跑失败了"，
        // timedOut = true 是"**根本没跑完就被掐了**" —— 后者指向环境/死循环，
        // 前者指向测试没过。混成一个字段，agent 会去改本来没问题的地方。
        out->Set(L"timedOut", json::Value::MakeBool(job.timedOut));
        out->Set(L"timeoutMs", json::Value::MakeInt(static_cast<int>(job.timeoutMs)));
        if (!job.error.empty()) out->Set(L"error", Str(job.error));
        return true;
    });

    regR(CmdId::DevRollback, [](const Args& args, json::Value* out) {
        const std::wstring path = str::Trim(args.Str(L"path"));
        const std::wstring snapshotId = str::Trim(args.Str(L"snapshotId"));
        // snapshotId 是**必填**：没有快照就没有退路，而回滚本身会覆盖工作树内容。
        // 双门只防"未授权"，防不了"授权后不可逆" —— 所以在这里就要挡住。
        if (snapshotId.empty()) {
            CommandBus::Get().NoteFailure(
                L"回滚会覆盖工作树内容，必须先 dev.snapshot 拿到 snapshotId 再回滚"
                L"（没有快照就没有退路）");
            return false;
        }
        if (!path.empty() && !devtools::ValidRepoPath(path, nullptr)) {
            CommandBus::Get().NoteFailure(L"路径不合法（只接受仓库内相对路径）");
            return false;
        }
        std::wstring before, after, err;
        if (!devsnap::Restore(snapshotId, path, &before, &after, &err)) {
            CommandBus::Get().NoteFailure(err.empty() ? L"回滚失败" : err);
            return false;
        }
        out->Set(L"path", Str(path));
        out->Set(L"snapshotId", Str(snapshotId));
        out->Set(L"before", Str(before));
        out->Set(L"after", Str(after));
        return true;
    });
}
