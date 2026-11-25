// 自我迭代命令族：dev.archive.decide / record / list。
//
// 【为什么 decide 是纯只读】
// 接受器必须能被反复复核：给定同一组配对试验，任何人（人或机器）都该得到同一个
// 判定。把判定做成纯函数 + 只读命令，才谈得上"可审计的接受" —— 否则"分数涨了就留"
// 会藏在一个有副作用的命令里，没法单独测。
//
// 【record 的证据来源】
// 原始轨迹由调用方给出（来自 dev.test 的 job.output），archive 不自己跑测试 ——
// 跑测试是 dev.test 的职责，而且评估面完整性由 dev.test 那个入口把关。
#include "app.h"

#include <string>
#include <vector>

#include "app_commands.h"
#include "dev_archive.h"
#include "eval_surface.h"
#include "text_file.h"

namespace {

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

json::Value DecisionJson(const devarchive::DecideResult& d) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"accepted", json::Value::MakeBool(d.accepted));
    v.Set(L"decision", Str(d.decision));
    v.Set(L"eProcess", json::Value::MakeNum(d.eProcess));
    v.Set(L"threshold", json::Value::MakeNum(d.threshold));
    v.Set(L"alpha", json::Value::MakeNum(devarchive::kAlpha));
    v.Set(L"lambda", json::Value::MakeNum(devarchive::kLambda));
    v.Set(L"trials", json::Value::MakeInt(d.trials));
    v.Set(L"wins", json::Value::MakeInt(d.wins));
    v.Set(L"ties", json::Value::MakeInt(d.ties));
    v.Set(L"losses", json::Value::MakeInt(d.losses));
    v.Set(L"invalid", json::Value::MakeInt(d.invalid));
    v.Set(L"reason", Str(d.reason));
    return v;
}

json::Value ScoreJson(const devarchive::Score& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"passed", json::Value::MakeInt(s.passed));
    v.Set(L"failed", json::Value::MakeInt(s.failed));
    v.Set(L"compileFail", json::Value::MakeInt(s.compileFail));
    v.Set(L"notRun", json::Value::MakeInt(s.notRun));
    v.Set(L"value", json::Value::MakeNum(s.value));
    return v;
}

json::Value FunnelJson(const devarchive::FunnelResult& f) {
    json::Value a = json::Value::MakeArray();
    for (const devarchive::StageResult& s : f.stages) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"stage", Str(s.stage));
        v.Set(L"passed", json::Value::MakeBool(s.passed));
        v.Set(L"reason", Str(s.reason));
        a.arr.push_back(std::move(v));
    }
    json::Value v = json::Value::MakeObject();
    v.Set(L"ok", json::Value::MakeBool(f.ok));
    v.Set(L"stages", std::move(a));
    return v;
}

}  // namespace

void App::RegisterArchiveCommands() {
    auto regR = cmd_reg::RegisterWithResult;

    regR(CmdId::DevArchiveDecide, [](const Args& args, json::Value* out) {
        const json::Value* trials = args.Any(L"trials");
        if (!trials || !trials->IsArray()) {
            CommandBus::Get().NoteFailure(L"trials 必须是数组（配对试验列表）");
            return false;
        }
        const devarchive::DecideResult d = devarchive::Decide(*trials);
        if (!d.ok) {
            CommandBus::Get().NoteFailure(d.reason.empty() ? L"判定失败" : d.reason);
            return false;
        }
        *out = DecisionJson(d);
        return true;
    });

    regR(CmdId::DevArchiveRecord, [](const Args& args, json::Value* out) {
        devarchive::RecordOptions opt;
        opt.id = args.Str(L"candidateId");
        opt.parentId = args.Str(L"parentId");
        opt.baselineId = args.Str(L"baselineId");
        opt.note = args.Str(L"note");
        const std::string raw = tf::ToUtf8(args.Str(L"trajectory"));
        opt.rawTrajectory = raw;

        // repeats 是字符串数组；Args 只有 StrArray，这里按 JSON 原样取，
        // 非字符串项直接拒绝 —— 不让"数字被静默转成 0"混进轨迹。
        if (const json::Value* r = args.Any(L"repeats")) {
            if (!r->IsArray()) {
                CommandBus::Get().NoteFailure(L"repeats 必须是字符串数组");
                return false;
            }
            for (const json::Value& v : r->arr) {
                if (!v.IsString()) {
                    CommandBus::Get().NoteFailure(L"repeats 必须是字符串数组");
                    return false;
                }
                opt.repeatTrajectories.push_back(tf::ToUtf8(v.str));
            }
        }

        // 评估面守卫：改了构建器/测试脚本的候选一律进不了 archive。
        const evalsurface::Report ev = evalsurface::Check();
        if (!ev.ok) {
            CommandBus::Get().NoteFailure(
                L"评估面已被改动，拒绝记录（改判据的候选不是改进）：" + ev.reason);
            return false;
        }
        opt.protectedChanged = ev.changed;

        const devarchive::RecordResult r = devarchive::Record(opt);
        if (!r.ok) {
            CommandBus::Get().NoteFailure(r.error.empty() ? L"记录失败" : r.error);
            return false;
        }
        out->Set(L"id", Str(r.id));
        out->Set(L"dir", Str(r.dir));
        out->Set(L"accepted", json::Value::MakeBool(r.decision.accepted && r.funnel.ok));
        out->Set(L"decision", DecisionJson(r.decision));
        out->Set(L"score", ScoreJson(r.score));
        out->Set(L"funnel", FunnelJson(r.funnel));
        out->Set(L"files", json::Value::MakeInt(r.files));
        return true;
    });

    regR(CmdId::DevArchiveList, [](const Args& args, json::Value* out) {
        const int limit = args.Int(L"limit", 100);
        if (limit < 1 || limit > 500) {
            CommandBus::Get().NoteFailure(L"limit 必须在 1..500 之间");
            return false;
        }
        const devarchive::ListResult l = devarchive::List(L"");
        json::Value a = json::Value::MakeArray();
        int n = 0;
        for (const devarchive::Entry& e : l.entries) {
            if (n++ >= limit) break;
            json::Value v = json::Value::MakeObject();
            v.Set(L"id", Str(e.id));
            v.Set(L"createdAt", Str(e.createdAt));
            v.Set(L"parentId", Str(e.parentId));
            v.Set(L"baselineId", Str(e.baselineId));
            v.Set(L"score", ScoreJson(e.score));
            v.Set(L"accepted", json::Value::MakeBool(e.accepted));
            v.Set(L"eProcess", json::Value::MakeNum(e.eProcess));
            v.Set(L"funnelOk", json::Value::MakeBool(e.funnelOk));
            v.Set(L"files", json::Value::MakeInt(e.files));
            a.arr.push_back(std::move(v));
        }
        json::Value skipped = json::Value::MakeArray();
        for (const std::wstring& id : l.skipped) skipped.arr.push_back(Str(id));
        out->Set(L"entries", std::move(a));
        out->Set(L"total", json::Value::MakeInt(static_cast<int>(l.entries.size())));
        out->Set(L"skipped", std::move(skipped));
        return true;
    });
}
