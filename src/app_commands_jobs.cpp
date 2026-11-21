// 作业域的命令层实现（接口说明见 app_commands_jobs.h）。
//
// 这里同时放三样东西，因为它们是同一件事的三个面：
//   · 共用入口的实现（起作业 / 等作业 / 收结果）；
//   · tool.job.get / tool.job.cancel / tool.job.list 三条查询与控制命令；
//   · 作业事件接线（进度与完成推给事件订阅者）。
// 拆开的话，"作业跑完了怎么告诉外界"就得在两个文件之间传状态。
#include "app.h"

#include "ai_events.h"
#include "app_commands.h"
#include "app_commands_jobs.h"
#include "job_ui.h"

namespace appjobs {
namespace {

// join 的兜底时限：作业跑完后还要留点时间收尾（写结果、join 工作线程）。
// 正常情况下作业早已结束，这只是防"万一" —— 宁可少等一会儿，
// 也不能让界面永远停在这个循环里。
constexpr unsigned kJoinGraceMs = 5000;

}  // namespace

unsigned long long ClampBudget(int ms) {
    if (ms < 100) return 100;
    if (ms > 30000) return 30000;
    return static_cast<unsigned long long>(ms);
}

unsigned long long ResolveBudget(bool hasBudget, int budgetMs, bool async, bool human) {
    if (hasBudget) return ClampBudget(budgetMs);
    // 异步与人工路径不限时；机器同步路径保持 1500ms（理由见 app_commands_jobs.h）。
    if (async || human) return scanctl::kUnlimitedBudget;
    return 1500;
}

std::wstring RunTask(HWND owner, const Theme& theme, const TaskSpec& spec, job::Body body) {
    const std::wstring id = job::Service::Get().Start(spec.kind, spec.target, std::move(body));
    if (id.empty()) return std::wstring();
    if (spec.async) return id;   // 不等：调用方自己去查

    if (spec.human) {
        // 人工路径：弹进度窗口，用户随时能按"取消"。
        jobui::WaitWithWindow(owner, theme, spec.title, id);
    } else {
        // 机器路径：不弹窗（没人盯着屏幕，却把界面锁住比冻结好不到哪去），
        // 但照样泵消息，界面不会僵死。
        jobui::WaitQuiet(id, spec.waitMs);
    }
    // join：作业线程捕获了 Service 的地址，不 join 就让 Service 析构 = 崩。
    job::Service::Get().WaitFinished(id, kJoinGraceMs);
    return id;
}

bool CollectResult(const std::wstring& jobId, bool async, json::Value* result,
                   std::wstring* err, FailKind* kind) {
    if (async) {
        // 异步：只回作业 id 与状态。调用方用 tool.job.get 取最终结果。
        if (result) {
            result->Set(L"jobId", json::Value::MakeStr(jobId));
            result->Set(L"state", json::Value::MakeStr(L"running"));
        }
        return true;
    }
    job::Status st;
    if (!job::Service::Get().Get(jobId, &st)) {
        if (err) *err = L"找不到作业：" + jobId;
        if (kind) *kind = FailKind::PreconditionUnmet;
        return false;
    }
    if (!st.done) {
        // 没等到：如实说"没跑完"，不假装有结果。
        if (err) *err = L"作业在等待时限内没有结束（它仍在后台运行，可用 tool.job.get 继续查）";
        // 时限到了 ≠ 出错了。下一步是"去查它"，不是"重试"也不是"改目标"，
        // 所以单给一个码，别混进 precondition_unmet。
        if (kind) *kind = FailKind::TimedOut;
        if (result) {
            result->Set(L"jobId", json::Value::MakeStr(jobId));
            result->Set(L"state", json::Value::MakeStr(L"running"));
            result->Set(L"complete", json::Value::MakeBool(false));
        }
        return false;
    }
    if (st.cancelled) {
        // 被取消：**不是失败，也没做完**。
        // 以前这里不判 cancelled，被取消的作业会落进下面的成功分支，
        // 把空结果当成"跑完了"报出去 —— 那比直接报错更坏（调用方会以为
        // "确实没有重复文件"）。
        if (err) *err = L"作业已被取消（用户中止）";
        if (kind) *kind = FailKind::CanceledByUser;
        if (result) {
            result->Set(L"jobId", json::Value::MakeStr(jobId));
            result->Set(L"state", json::Value::MakeStr(L"cancelled"));
            result->Set(L"complete", json::Value::MakeBool(false));
            result->Set(L"cancelled", json::Value::MakeBool(true));
        }
        return false;
    }
    if (st.failed) {
        if (err) *err = st.error.empty() ? std::wstring(L"作业失败") : st.error;
        if (kind) *kind = FailKind::PreconditionUnmet;
        return false;
    }
    if (result) *result = st.result;
    return true;
}

}  // namespace appjobs

namespace {

// 把作业状态摊平成 JSON（tool.job.get 用）。
// result 只在**成功完成**时带上 —— 半成品结果比没有结果更危险。
void FillJobStatus(const job::Status& st, json::Value* out) {
    if (!out) return;
    out->Set(L"id", json::Value::MakeStr(st.id));
    out->Set(L"kind", json::Value::MakeStr(st.kind));
    out->Set(L"target", json::Value::MakeStr(st.target));
    out->Set(L"done", json::Value::MakeBool(st.done));
    out->Set(L"cancelled", json::Value::MakeBool(st.cancelled));
    out->Set(L"failed", json::Value::MakeBool(st.failed));
    out->Set(L"stage", json::Value::MakeStr(st.stage));
    out->Set(L"doneCount", json::Value::MakeNum(static_cast<double>(st.doneCount)));
    out->Set(L"totalCount", json::Value::MakeNum(static_cast<double>(st.totalCount)));
    if (!st.error.empty()) out->Set(L"error", json::Value::MakeStr(st.error));
    if (st.done && !st.failed && !st.cancelled) out->Set(L"result", st.result);
}

// 作业事件接线：进度与完成都推给事件订阅者（与 dev.build / dev.test 同一范式）。
//
// 【为什么作业要发事件】调用方拿到 jobId 之后只有两个选择 —— 轮询，或等推送。
// 轮询把"多久知道结果"的责任推给调用方（还得自己定间隔）；
// 推送让订阅者一收到完成事件就知道结果好了。
//
// 两个钩子都在**工作线程**上被调用，所以事件发布必须线程安全
// （ai::events::Publish 是线程安全的）。
void WireJobEvents() {
    job::Service& svc = job::Service::Get();
    svc.SetProgressHook([](const job::Status& st) {
        json::Value data = json::Value::MakeObject();
        data.Set(L"id", json::Value::MakeStr(st.id));
        data.Set(L"kind", json::Value::MakeStr(st.kind));
        data.Set(L"stage", json::Value::MakeStr(st.stage));
        data.Set(L"doneCount", json::Value::MakeNum(static_cast<double>(st.doneCount)));
        data.Set(L"totalCount", json::Value::MakeNum(static_cast<double>(st.totalCount)));
        ai::events::Publish(L"tool.job.progress", data);
    });
    svc.SetDoneHook([](const job::Status& st) {
        json::Value data = json::Value::MakeObject();
        data.Set(L"id", json::Value::MakeStr(st.id));
        data.Set(L"kind", json::Value::MakeStr(st.kind));
        data.Set(L"target", json::Value::MakeStr(st.target));
        data.Set(L"cancelled", json::Value::MakeBool(st.cancelled));
        data.Set(L"failed", json::Value::MakeBool(st.failed));
        if (!st.error.empty()) data.Set(L"error", json::Value::MakeStr(st.error));
        // 完成事件**带结果**：否则订阅者收到"做完了"还要再调一次才拿到东西，
        // 那就等于把轮询换成了两段式轮询。
        if (st.done && !st.failed && !st.cancelled) data.Set(L"result", st.result);
        ai::events::Publish(L"tool.job.done", data);
    });
}

}  // namespace

void App::RegisterJobCommands() {
    auto regR = cmd_reg::RegisterWithResult;

    // 作业事件接线放在这里：注册命令与接线本来就是同一件事的两面
    // —— 有作业命令，就该有作业事件。
    WireJobEvents();

    // tool.job.get：查一个作业的状态与结果。
    regR(CmdId::ToolJobGet, [](const Args& args, json::Value* result) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少参数 id（作业 id）");
            return false;
        }
        job::Status st;
        if (!job::Service::Get().Get(id, &st)) {
            CommandBus::Get().NoteFailure(L"找不到作业：" + id);
            return false;
        }
        FillJobStatus(st, result);
        return true;
    });

    // tool.job.cancel：叫停一个还在跑的作业。
    //
    // 【为什么它是"请求"而不是"保证停下】取消是协作式的：置一个原子位，
    // 数据层在遍历循环里查。已经被系统调用卡住的那一步（读一个被独占的文件）
    // 中断不了 —— 这是诚实的边界，不是缺陷。所以返回的是"已受理取消"。
    regR(CmdId::ToolJobCancel, [](const Args& args, json::Value* result) {
        const std::wstring id = args.Str(L"id");
        if (id.empty()) {
            CommandBus::Get().NoteFailure(L"缺少参数 id（作业 id）");
            return false;
        }
        if (!job::Service::Get().Cancel(id)) {
            CommandBus::Get().NoteFailure(L"作业不存在或已经结束：" + id);
            return false;
        }
        result->Set(L"id", json::Value::MakeStr(id));
        result->Set(L"cancelRequested", json::Value::MakeBool(true));
        return true;
    });

    // tool.job.list：列出全部作业（新的在前）。
    //
    // 不带结果体 —— 作业结果可能很大（重复文件清单、差异清单），
    // 列表接口只给摘要，要结果用 tool.job.get。
    regR(CmdId::ToolJobList, [](const Args&, json::Value* result) {
        json::Value arr = json::Value::MakeArray();
        for (const auto& st : job::Service::Get().List()) {
            json::Value o = json::Value::MakeObject();
            o.Set(L"id", json::Value::MakeStr(st.id));
            o.Set(L"kind", json::Value::MakeStr(st.kind));
            o.Set(L"target", json::Value::MakeStr(st.target));
            o.Set(L"done", json::Value::MakeBool(st.done));
            o.Set(L"cancelled", json::Value::MakeBool(st.cancelled));
            o.Set(L"failed", json::Value::MakeBool(st.failed));
            o.Set(L"stage", json::Value::MakeStr(st.stage));
            o.Set(L"doneCount", json::Value::MakeNum(static_cast<double>(st.doneCount)));
            o.Set(L"totalCount", json::Value::MakeNum(static_cast<double>(st.totalCount)));
            arr.arr.push_back(std::move(o));
        }
        result->Set(L"jobs", arr);
        result->Set(L"count", json::Value::MakeInt(static_cast<int>(arr.arr.size())));
        return true;
    });
}

void App::ShutdownJobs() {
    // 长任务作业：先摘钩子（窗口没了不该再发事件），再取消并 join 所有工作线程。
    // 不 join 的话，工作线程会往已析构的 Service 上写 —— 崩在退出那一刻，
    // 而且崩得莫名其妙（用户只看到"关窗口时闪退"）。
    job::Service& jobs = job::Service::Get();
    jobs.SetProgressHook(nullptr);
    jobs.SetDoneHook(nullptr);
    jobs.Shutdown();
}