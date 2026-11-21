// 开发面工具实现（接口说明见 dev_tools.h）。
#include "dev_tools.h"

#include "ai_events.h"
#include "str.h"
#include "text_file.h"
#include "common.h"
#include "config.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

namespace devtools {
namespace {

std::mutex g_mu;
std::map<std::wstring, Job> g_jobs;
unsigned long long g_nextId = 0;
constexpr size_t kMaxJobs = 64;
constexpr size_t kMaxOutput = 256 * 1024;

std::wstring Quote(const std::wstring& s) {
    return L"\"" + std::wstring(s) + L"\"";
}

// 在仓库目录里跑一条命令，返回 (退出码, 输出)。
// 用 cd /d 而不是 SetCurrentDirectory：那是进程级状态，后台线程改它会让 UI 受影响。
bool RunCapture(const std::wstring& command, int* code, std::wstring* output) {
    const std::wstring repo = tf::ExeDir();
    const std::wstring full = L"cd /d " + Quote(repo) + L" && " + command + L" 2>&1";
    FILE* f = _wpopen(full.c_str(), L"rb");
    if (!f) {
        if (code) *code = -1;
        if (output) output->clear();
        return false;
    }
    std::string bytes;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (bytes.size() + n > kMaxOutput) {
            bytes.append(buf, kMaxOutput - bytes.size());
            break;
        }
        bytes.append(buf, n);
    }
    const int rc = _pclose(f);
    if (code) *code = rc;
    if (output) *output = str::FromUtf8(bytes);
    return true;
}

std::wstring TrimTail(const std::wstring& s) {
    return s.size() <= 4000 ? s : s.substr(s.size() - 4000);
}

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

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

std::wstring StartJob(const std::wstring& kind, const std::wstring& command) {
    Job job;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        job.id = L"job-" + std::to_wstring(++g_nextId);
        job.kind = kind;
        job.command = command;
        g_jobs[job.id] = job;
        while (g_jobs.size() > kMaxJobs) {
            // 只淘汰已完成的，运行中的作业不能被挤掉（否则结果永远查不到）。
            auto it = g_jobs.begin();
            while (it != g_jobs.end() && !it->second.done) ++it;
            if (it == g_jobs.end()) break;
            g_jobs.erase(it);
        }
    }
    const std::wstring id = job.id;
    std::thread([id, kind, command] {
        Job done;
        done.id = id;
        done.kind = kind;
        done.command = command;
        int code = -1;
        std::wstring out;
        if (RunCapture(command, &code, &out)) {
            done.exitCode = code;
            done.output = out;
        } else {
            done.error = L"无法启动进程";
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
        data.Set(L"ok", json::Value::MakeBool(code == 0));
        data.Set(L"output", Str(TrimTail(done.output)));
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
    return true;
}

std::wstring Snapshot() {
    int code = 0;
    std::wstring head, status, diff, bash, python;
    RunCapture(L"git rev-parse --short HEAD", &code, &head);
    RunCapture(L"git status --short", &code, &status);
    RunCapture(L"git diff --stat", &code, &diff);
    RunCapture(L"where bash", &code, &bash);
    RunCapture(L"where python", &code, &python);

    json::Value v = json::Value::MakeObject();
    v.Set(L"head", Str(str::Trim(head)));
    v.Set(L"status", Str(status));
    v.Set(L"diffStat", Str(diff));
    v.Set(L"repo", Str(tf::ExeDir()));
    v.Set(L"profile", Str(Config::Get().Dir()));
    v.Set(L"bash", Str(str::Trim(bash)));
    v.Set(L"python", Str(str::Trim(python)));
    if (head.empty()) v.Set(L"error", Str(L"git rev-parse 没有输出（仓库不可用？）"));
    return json::Stringify(v, 0);
}

bool Rollback(const std::wstring& rel, std::wstring* before, std::wstring* after,
              std::wstring* err) {
    if (!ValidRepoPath(rel, err)) return false;
    int code = 0;
    std::wstring status;
    // 未修改的文件不给回滚：避免把"本来就没动"包装成一次成功操作。
    RunCapture(L"git status --short -- " + Quote(rel), &code, &status);
    if (str::Trim(status).empty()) {
        if (err) *err = L"该文件没有未提交的改动，无需回滚（或路径写错）";
        return false;
    }
    RunCapture(L"git hash-object " + Quote(rel), &code, before);
    RunCapture(L"git checkout -- " + Quote(rel), &code, &status);
    if (code != 0) {
        if (err) *err = L"git checkout 失败：" + str::Trim(status);
        return false;
    }
    RunCapture(L"git hash-object " + Quote(rel), &code, after);
    return true;
}

}  // namespace devtools
