// aiqueue 的实现。接口与不变式说明见 ai_queue.h。
#include "ai_queue.h"

#include <chrono>

namespace aiqueue {

bool Queue::Submit(const std::wstring& request, int waitMs, std::wstring* response) {
    // 请求对象是**堆上**的共享所有权：提交方、队列、UI 线程各持一份引用。
    // 这样提交方超时走人时，UI 线程那份引用仍然保证对象活着 ——
    // 它之后往 job->response 写结果不会碰到已释放的内存。
    auto job = std::make_shared<Job>();
    job->request = request;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(job);
    }
    cv_.notify_one();

    std::unique_lock<std::mutex> lk(mu_);
    const bool got = cv_.wait_for(lk, std::chrono::milliseconds(waitMs),
                                  [&] { return job->done || interrupted_; });
    if (!job->done) {
        // 超时（或停机打断）：把它从队列里摘掉（还没被 UI 线程取走的话）。
        // 【关键】这里**不能**让 job 析构 —— UI 线程手里可能还有引用。
        // 放弃的只是"等结果"这件事，请求本身仍会被安全地处理完。
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (*it == job) { queue_.erase(it); break; }
        }
        job->abandoned = true;
        if (response) response->clear();
        return false;
    }
    if (response) *response = job->response;
    return true;
}

int Queue::Pump(const Handler& handler) {
    int n = 0;
    for (;;) {
        JobPtr job;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.empty()) break;
            job = queue_.front();
            queue_.pop_front();
        }
        // job 这份引用保证对象活到本次处理结束 ——
        // 即使提交方已经超时走人（它那份引用早放了）。
        std::wstring resp = handler ? handler(job->request) : std::wstring();
        {
            std::lock_guard<std::mutex> lk(mu_);
            job->response = std::move(resp);
            job->done = true;
            // 客户端早走了：这个请求跑完了但没人要结果 —— 计一笔，
            // 用来回答"为什么界面卡了那么久"。
            if (job->abandoned) ++abandoned_;
        }
        cv_.notify_all();
        ++n;
    }
    return n;
}

void Queue::Interrupt() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        interrupted_ = true;
        queue_.clear();
    }
    // 唤醒所有还在等的提交方：他们立刻拿到 false，不必干等到超时。
    cv_.notify_all();
}

void Queue::Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    interrupted_ = false;
    queue_.clear();
}

size_t Queue::Pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

size_t Queue::Abandoned() const {
    std::lock_guard<std::mutex> lk(mu_);
    return abandoned_;
}

}  // namespace aiqueue
