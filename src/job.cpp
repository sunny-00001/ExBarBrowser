// 长任务作业服务实现（接口说明见 job.h）。
//
// 【为什么自己写而不用 std::async】std::async 的返回对象析构时会**阻塞等待**
// 任务结束 —— 那正好把"作业要在命令返回后继续跑"这件事否掉了。
// 而且它没有取消、没有进度、也没有"作业 id"：这里三样都要。
#include "job.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace job {
namespace {

using Clock = std::chrono::steady_clock;

// 进度推送的最小间隔。进度变化很密（遍历时每 1024 项一次也不算慢），
// 全推会把事件环形缓冲（512 条）挤爆 —— 真正重要的"作业完成"事件
// 反而被挤掉。200ms 足够跟手，也不会淹没历史。
constexpr int kProgressIntervalMs = 200;

}  // namespace

struct Service::Record {
    std::wstring id;
    std::wstring kind;
    std::wstring target;
    std::atomic<bool> cancel{false};
    bool done = false;
    bool cancelled = false;
    bool failed = false;
    std::wstring error;
    std::wstring stage;
    unsigned long long doneCount = 0;
    unsigned long long totalCount = 0;
    json::Value result = json::Value::MakeObject();
    std::thread worker;
    bool joined = false;
    Clock::time_point lastPush{};
    bool pushedOnce = false;
};

Service& Service::Get() {
    // 函数内静态：与 Config::Get() 同一范式（进程级服务状态），
    // 而不是文件级裸全局变量。
    static Service inst;
    return inst;
}

Service::~Service() {
    Shutdown();
}

Status Service::SnapshotLocked(const Record& rec) const {
    Status s;
    s.id = rec.id;
    s.kind = rec.kind;
    s.target = rec.target;
    s.done = rec.done;
    s.cancelled = rec.cancelled;
    s.failed = rec.failed;
    s.error = rec.error;
    s.stage = rec.stage;
    s.doneCount = rec.doneCount;
    s.totalCount = rec.totalCount;
    s.result = rec.result;
    return s;
}

std::wstring Service::Start(const std::wstring& kind, const std::wstring& target, Body body) {
    if (!body) return std::wstring();

    auto rec = std::make_shared<Record>();
    rec->kind = kind;
    rec->target = target;

    // 钩子在启动时**取一份拷贝**再捕获：钩子本身是进程级配置，
    // 运行时改它（SetDoneHook）不该影响已经在跑的作业，
    // 也不该让工作线程无锁地读一个可能被改写的 std::function。
    std::function<void(const Status&)> progressHook;
    std::function<void(const Status&)> doneHook;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rec->id = L"job" + std::to_wstring(nextId_++);
        progressHook = progressHook_;
        doneHook = doneHook_;
    }

    scanctl::Control ctl;
    ctl.cancel = &rec->cancel;
    ctl.onProgress = [this, rec, progressHook](const std::wstring& stage,
                                               unsigned long long done,
                                               unsigned long long total) {
        Status st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            rec->stage = stage;
            rec->doneCount = done;
            rec->totalCount = total;
            const Clock::time_point now = Clock::now();
            if (rec->pushedOnce &&
                now - rec->lastPush < std::chrono::milliseconds(kProgressIntervalMs))
                return;
            rec->lastPush = now;
            rec->pushedOnce = true;
            st = SnapshotLocked(*rec);
        }
        if (progressHook) progressHook(st);
    };

    rec->worker = std::thread([this, rec, body, ctl, doneHook]() mutable {
        json::Value out = json::Value::MakeObject();
        std::wstring err;
        const bool ok = body(ctl, &out, &err);
        Status st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            rec->done = true;
            // 取消是"人/机器说了停"，与"失败"是两回事：
            // 被取消的作业没有错误，只是没做完 —— 混成 failed 会让
            // 调用方以为出了错而重试。
            rec->cancelled = rec->cancel.load(std::memory_order_relaxed);
            rec->failed = !ok;
            rec->error = err;
            if (ok) rec->result = std::move(out);
            st = SnapshotLocked(*rec);
        }
        cv_.notify_all();
        if (doneHook) doneHook(st);
    });

    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_.push_back(rec);
    }
    return rec->id;
}

bool Service::Get(const std::wstring& id, Status* out) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& rec : jobs_) {
        if (rec->id != id) continue;
        if (out) *out = SnapshotLocked(*rec);
        return true;
    }
    return false;
}

bool Service::Cancel(const std::wstring& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& rec : jobs_) {
        if (rec->id != id) continue;
        if (rec->done) return false;   // 已经收工，取消没有意义
        rec->cancel.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

std::vector<Status> Service::List() {
    std::vector<Status> out;
    std::lock_guard<std::mutex> lk(mu_);
    // 新的在前：调用方多半关心"刚起的那个"。
    for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) out.push_back(SnapshotLocked(**it));
    return out;
}

void Service::SetProgressHook(std::function<void(const Status&)> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    progressHook_ = std::move(fn);
}

void Service::SetDoneHook(std::function<void(const Status&)> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    doneHook_ = std::move(fn);
}

bool Service::WaitFinished(const std::wstring& id, unsigned timeoutMs) {
    std::shared_ptr<Record> rec;
    {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = jobs_.end();
        for (auto j = jobs_.begin(); j != jobs_.end(); ++j) {
            if ((*j)->id == id) { it = j; break; }
        }
        if (it == jobs_.end()) return false;
        rec = *it;
        if (timeoutMs == 0) {
            cv_.wait(lk, [&rec] { return rec->done; });
        } else if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                 [&rec] { return rec->done; })) {
            return false;
        }
    }
    // join 必须在**锁外**做：工作线程收工时要拿同一把锁写状态，
    // 拿着锁 join 就是自己等自己。
    JoinRecord(rec);
    return true;
}

void Service::JoinRecord(const std::shared_ptr<Record>& rec) {
    if (!rec) return;
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec->joined) return;
        rec->joined = true;
        t = std::move(rec->worker);
    }
    if (t.joinable()) t.join();
}

void Service::Shutdown() {
    std::vector<std::shared_ptr<Record>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& rec : jobs_) {
            rec->cancel.store(true, std::memory_order_relaxed);
            all.push_back(rec);
        }
    }
    for (auto& rec : all) JoinRecord(rec);
}

}  // namespace job
