// 开发面工具实现（接口说明见 dev_tools.h）。
#include "dev_tools.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai_events.h"
#include "dev_proc.h"
#include "dev_snapshot.h"
#include "eval_surface.h"
#include "str.h"
#include "text_file.h"
#include "common.h"
#include "config.h"

namespace devtools {
namespace {

std::mutex g_mu;
std::map<std::wstring, Job> g_jobs;
unsigned long long g_nextId = 0;
constexpr size_t kMaxJobs = 64;

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

// 在仓库目录里跑一条命令，返回 (退出码, 输出)。
// 用 CreateProcess + 参数数组，不经 shell（理由见 dev_proc.h）。
bool RunCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                int* code, std::wstring* output) {
    std::wstring err;
    return devproc::RunCapture(program, args, tf::ExeDir(), code, output, &err);
}

// 输出裁剪：**头 + 尾**都留。
// 只留尾巴会把第一处编译错误丢掉 —— 而那恰恰是最该先看的一行；
// 只留头会把最后的汇总（"N errors generated"）丢掉。
std::wstring TrimOutput(const std::wstring& s, bool* truncated) {
    constexpr size_t kKeep = 4000;
    if (s.size() <= kKeep) {
        if (truncated) *truncated = false;
        return s;
    }
    constexpr size_t kHead = 1400;
    const size_t tail = kKeep - kHead - 48;
    std::wstring marker = L"\n…［中间省略 " + std::to_wstring(s.size() - kHead - tail) +
                          L" 字符］…\n";
    if (truncated) *truncated = true;
    return s.substr(0, kHead) + marker + s.substr(s.size() - tail);
}

// git 的四个只读查询：统一走 argv，不拼字符串。
std::wstring Git(const std::wstring& sub, const std::vector<std::wstring>& rest) {
    std::vector<std::wstring> args;
    args.push_back(sub);
    for (const std::wstring& a : rest) args.push_back(a);
    int code = 0;
    std::wstring out;
    RunCapture(L"git", args, &code, &out);
    return out;
}

// 在 PATH 里找可执行文件（原来是 `where bash`，那条要过 cmd.exe）。
// 找不到返回空串。
std::wstring FindExe(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf, n);
}

}  // namespace

bool ValidRepoPath(const std::wstring& rel, std::wstring* err) {
    auto reject = [&](const wchar_t* why) {
        if (err) *err = why;
        return false;
    };
    if (rel.empty()) return reject(L"路径为空");
    if (rel.size() > 1 && rel[1] == L':') return reject(L"不接受绝对路径（仓库内相对路径即可）");
    if (rel[0] == L'\\' || rel[0] == L'/') return reject(L"不接受根目录开始的路径");
    if (rel.find(L'"') != std::wstring::npos) return reject(L"路径含引号");
    for (const wchar_t* bad : {L"&", L"|", L"<", L">", L"^"}) {
        if (rel.find(bad) != std::wstring::npos) return reject(L"路径含命令分隔符");
    }
    size_t start = 0;
    for (;;) {
        const size_t end = rel.find_first_of(L"\\/", start);
        const std::wstring part = rel.substr(start, end == std::wstring::npos
                                                      ? std::wstring::npos : end - start);
        if (part == L"..") return reject(L"路径含 ..，可能跳出仓库");
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

std::wstring StartJob(const std::wstring& kind, const std::wstring& program,
                       const std::vector<std::wstring>& args,
                       unsigned long long timeoutMs) {
    Job job;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        job.id = L"job-" + std::to_wstring(++g_nextId);
        job.kind = kind;
        job.command = devproc::BuildCommandLine(program, args);
        job.startedAt = GetTickCount64();
        job.timeoutMs = timeoutMs;
        g_jobs[job.id] = job;
        // 淘汰顺序：先删已完成的；全都还在跑就把**最老的**踢掉，
        // 否则 64 个挂死作业会把队列堵死 —— 新作业登记不上，等于整条链废了。
        while (g_jobs.size() > kMaxJobs) {
            auto it = g_jobs.begin();
            while (it != g_jobs.end() && !it->second.done) ++it;
            if (it == g_jobs.end()) {
                it = g_jobs.begin();  // 没有已完成的 → 踢掉最老的
                LogDbg(L"[dev] 作业队列已满，淘汰最老的作业：%s", it->second.id.c_str());
            }
            g_jobs.erase(it);
        }
    }
    const std::wstring id = job.id;
    std::thread([id, kind, program, args, timeoutMs] {
        Job done;
        done.id = id;
        done.kind = kind;
        done.command = devproc::BuildCommandLine(program, args);
        done.timeoutMs = timeoutMs;
        int code = -1;
        std::wstring out;
        // 硬超时在这里生效：到点杀掉整棵进程树，然后按失败收场。
        // "被超时杀掉"要能被调用方分辨 —— 它和"跑失败了"是两种完全不同的结论，
        // 前者说明**代码/环境有问题**，后者只说明这次没通过。
        if (devproc::RunCaptureTimeout(program, args, tf::ExeDir(), timeoutMs, &code, &out,
                                       &done.error, &done.timedOut)) {
            done.exitCode = code;
            done.output = TrimOutput(out, &done.truncated);
        }
        if (done.timedOut) {
            done.error = L"作业超时（超过 " + std::to_wstring(timeoutMs / 60000) +
                         L" 分钟，已杀掉整个进程树）";
            LogDbg(L"[dev] 作业超时被杀：%s（%ls）", id.c_str(), kind.c_str());
        }
        done.done = true;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_jobs.find(id);
            if (it != g_jobs.end()) it->second = done;
        }
        json::Value data = json::Value::MakeObject();
        data.Set(L"id", Str(id));
        data.Set(L"kind", Str(kind));
        data.Set(L"exitCode", json::Value::MakeInt(code));
        data.Set(L"ok", json::Value::MakeBool(code == 0 && !done.timedOut));
        data.Set(L"output", Str(done.output));
        data.Set(L"truncated", json::Value::MakeBool(done.truncated));
        data.Set(L"timedOut", json::Value::MakeBool(done.timedOut));
        if (!done.error.empty()) data.Set(L"error", Str(done.error));
        ai::events::Publish(L"dev.job.done", data);
    }).detach();
    return id;
}

bool GetJob(const std::wstring& id, Job* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_jobs.find(id);
    if (it == g_jobs.end()) return false;
    if (out) *out = it->second;
    // 【为什么这里不再"按超时上报"】旧版会在读作业时现编一个"超时"结论 ——
    // 那是**假**：进程其实还在跑，只是我们不再等了。现在超时是**真杀**
    // （RunCaptureTimeout 到点杀整棵进程树），Job::timedOut 由执行侧如实写入。
    // 假超时比没超时更坏：agent 会以为"编译卡住了"，去改本来没问题的地方。
    return true;
}

// 解析 `git diff --numstat` 的一行："<added>\t<deleted>\t<path>"（二进制是 "-\t-\t..."）。
// 返回 path 与增删行数；非新增/删除的普通改动才有意义。
bool ParseNumstat(const std::wstring& line, std::wstring* path, int* added, int* deleted) {
    const size_t t1 = line.find(L'\t');
    if (t1 == std::wstring::npos) return false;
    const size_t t2 = line.find(L'\t', t1 + 1);
    if (t2 == std::wstring::npos) return false;
    const std::wstring a = line.substr(0, t1);
    const std::wstring d = line.substr(t1 + 1, t2 - t1 - 1);
    if (a == L"-" || d == L"-") return false;  // 二进制：行数无意义
    *added = _wtoi(a.c_str());
    *deleted = _wtoi(d.c_str());
    *path = line.substr(t2 + 1);
    return !path->empty();
}

// 逐文件的增删行数（numstat 只给 changed 文件，与 CurrentChanges 的路径并集用）。
// key 是仓库内相对路径（正斜杠）；numstat 给的路径已是正斜杠，无需再归一化。
std::map<std::wstring, std::pair<int, int>> DiffNumstat() {
    std::map<std::wstring, std::pair<int, int>> out;
    // 工作区 + 暂存区都算：dev.diff 是"从上次提交到现在改了什么"的完整视图。
    for (const auto& args : {
             std::vector<std::wstring>{L"-c", L"core.quotepath=false", L"diff", L"--numstat"},
             std::vector<std::wstring>{L"-c", L"core.quotepath=false", L"diff", L"--cached",
                                       L"--numstat"},
         }) {
        const std::wstring text = GitArgs(args);
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(start, end - start);
            while (!line.empty() && line.back() == L'\r') line.pop_back();
            std::wstring path;
            int added = 0, deleted = 0;
            if (ParseNumstat(line, &path, &added, &deleted)) {
                auto& slot = out[path];
                slot.first += added;
                slot.second += deleted;
            }
            if (end == text.size()) break;
            start = end + 1;
        }
    }
    return out;
}

std::wstring Diff() {
    json::Value v = json::Value::MakeObject();
    const std::wstring head = str::Trim(Git(L"rev-parse", {L"--short", L"HEAD"}));
    v.Set(L"head", Str(head));
    if (head.empty()) {
        v.Set(L"error", Str(L"git rev-parse 没有输出（仓库不可用？）"));
        return json::Stringify(v, 0);
    }

    const auto changes = devsnap::CurrentChanges();
    const auto numstat = DiffNumstat();
    json::Value files = json::Value::MakeArray();
    for (const auto& f : changes) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"path", Str(f.path));
        o.Set(L"tracked", json::Value::MakeBool(f.tracked));
        o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
        o.Set(L"mtime", Str(f.mtime));
        o.Set(L"sha256", Str(f.sha256));  // 空 = 没算出来（目录/已删），不是"内容是空的"
        auto it = numstat.find(f.path);
        o.Set(L"added", json::Value::MakeInt(it == numstat.end() ? 0 : it->second.first));
        o.Set(L"deleted", json::Value::MakeInt(it == numstat.end() ? 0 : it->second.second));
        files.arr.push_back(std::move(o));
    }
    v.Set(L"files", std::move(files));

    // patch 可能很大（一次大重构几 MB）——头+尾裁剪，中间的省略处留标记。
    // 需要完整 diff 的调用方可以自己跑 git diff：这里给的是**审阅用摘要**，不是存档。
    const std::wstring patch = GitArgs({L"-c", L"core.quotepath=false", L"diff",
                                        L"--no-ext-diff", L"--no-color"});
    const std::wstring staged = GitArgs({L"-c", L"core.quotepath=false", L"diff", L"--cached",
                                         L"--no-ext-diff", L"--no-color"});
    bool truncated = false;
    v.Set(L"patch", Str(TrimOutput(patch, &truncated)));
    bool stagedTruncated = false;
    v.Set(L"stagedPatch", Str(TrimOutput(staged, &stagedTruncated)));
    v.Set(L"patchTruncated", json::Value::MakeBool(truncated || stagedTruncated));
    v.Set(L"stat", Str(Git(L"diff", {L"--stat"})));
    return json::Stringify(v, 0);
}

std::wstring Snapshot() {
    const std::wstring head = Git(L"rev-parse", {L"--short", L"HEAD"});
    const std::wstring status = Git(L"status", {L"--short"});
    const std::wstring diff = Git(L"diff", {L"--stat"});
    const std::wstring bash = FindExe(L"bash.exe");
    const std::wstring python = FindExe(L"python.exe");

    const evalsurface::Report ev = evalsurface::Check();

    json::Value v = json::Value::MakeObject();
    v.Set(L"head", Str(str::Trim(head)));
    v.Set(L"status", Str(status));
    v.Set(L"diffStat", Str(diff));
    v.Set(L"repo", Str(tf::ExeDir()));
    v.Set(L"profile", Str(Config::Get().Dir()));
    v.Set(L"bash", Str(bash));
    v.Set(L"python", Str(python));
    v.Set(L"evalSurfaceOk", json::Value::MakeBool(ev.ok));
    v.Set(L"evalSurfaceManifest", Str(ev.manifestPath));
    if (!ev.changed.empty()) {
        json::Value arr = json::Value::MakeArray();
        for (const std::wstring& c : ev.changed) arr.arr.push_back(Str(c));
        v.Set(L"evalSurfaceChanged", arr);
    }
    if (!ev.reason.empty()) v.Set(L"evalSurfaceReason", Str(ev.reason));
    if (str::Trim(head).empty()) {
        v.Set(L"error", Str(L"git rev-parse 没有输出（仓库不可用？）"));
    }
    return json::Stringify(v, 0);
}

// 旧的 Rollback() 已删除 —— 语义搬到 devsnap::Restore()
// （见 dev_snapshot.h：回滚按快照还原，且回滚前的内容会被存下来，
//  所以回滚这一步本身可逆，不再是不可逆的 git checkout）。

}  // namespace devtools
