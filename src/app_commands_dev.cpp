// 开发面命令的处理器（命令表与接口见 commands.h / dev_tools.h）。
//
// 只做三件事：取参 → 调 devtools → 把结构化结果交给命令总线。
// 真正的进程管理/回滚校验在 dev_tools.cpp，命令行与 UI 都走那一份。
#include "app.h"

#include "app_commands.h"
#include "dev_tools.h"
#include "str.h"

namespace {

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

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
        *out = std::move(snapshot);
        return true;
    });

    regR(CmdId::DevBuild, [](const Args& args, json::Value* out) {
        // 构建器用 python build_fast.py：build.sh 只是它的薄壳，直接调底层
        // 可以少一层 bash 依赖（但仍然是同一份构建实现）。
        const std::wstring target = str::Trim(args.Str(L"target"));
        std::wstring command = L"python build_fast.py";
        if (!target.empty()) command += L" " + target;
        const std::wstring id = devtools::StartJob(L"build", command);
        out->Set(L"jobId", Str(id));
        return true;
    });

    regR(CmdId::DevTest, [](const Args& args, json::Value* out) {
        const std::wstring filter = str::Trim(args.Str(L"filter"));
        std::wstring command = L"bash tests/run_all.sh";
        if (!filter.empty()) command += L" " + filter;
        const std::wstring id = devtools::StartJob(L"test", command);
        out->Set(L"jobId", Str(id));
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
        if (!job.error.empty()) out->Set(L"error", Str(job.error));
        return true;
    });

    regR(CmdId::DevRollback, [](const Args& args, json::Value* out) {
        const std::wstring path = str::Trim(args.Str(L"path"));
        std::wstring before, after, err;
        if (!devtools::Rollback(path, &before, &after, &err)) {
            CommandBus::Get().NoteFailure(err.empty() ? L"回滚失败" : err);
            return false;
        }
        out->Set(L"path", Str(path));
        out->Set(L"before", Str(before));
        out->Set(L"after", Str(after));
        return true;
    });
}
