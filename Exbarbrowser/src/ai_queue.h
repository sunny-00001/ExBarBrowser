// 「提交 → UI 线程执行 → 等结果」的一次性请求队列。
//
// 【为什么单独一个文件】这段线程契约是最容易写错的一块：
// 谁拥有请求、提交方能等多久、超时之后谁还在用它 ——
// 写错了不是"功能不对"，而是**内存被写坏**（未定义行为，可能当场崩，
// 也可能悄悄改掉别人的数据）。收成一个类之后才能单独单测。
//
// 【核心不变式】请求对象在「提交方」与「UI 线程」之间**共享所有权**：
// 提交方超时走人时，UI 线程可能还在处理这个请求（handler 跑得久，
// 比如一次大目录的磁盘扫描）；对象必须活到 UI 线程放手为止。
//
// 【踩过的坑】早期实现把请求对象放在**提交方的栈上**（局部变量），
// 队列里存裸指针。于是出现这条路径：
//   提交方等待超时 → 返回 → **栈帧销毁**
//   → UI 线程这时才处理完，往 job->response 写结果
//   = 写已释放的栈（提交方栈帧已被复用，写到谁头上全看运气）。
// 现在请求对象是 shared_ptr：提交方、队列、UI 线程各持一份引用，
// 谁先放手都不影响对象寿命；超时放弃的只是"等结果"这件事，
// UI 线程仍然会把那个请求安全地处理完。
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace aiqueue {

class Queue {
public:
    // 处理一个请求：输入请求文本，输出响应文本。
    // 在**调用 Pump 的那个线程**上执行（UI 线程），所以它可以碰界面。
    using Handler = std::function<std::wstring(const std::wstring&)>;

    // 提交方（监听线程）调用：排队并等结果。
    //   waitMs    —— 最长等待毫秒数。
    //   response  —— 拿到结果时写入（可选）。
    // 返回 true  = 在 waitMs 内拿到了结果。
    // 返回 false = 超时：**没人再要结果了**，但 UI 线程仍会安全地处理完它。
    bool Submit(const std::wstring& request, int waitMs, std::wstring* response);

    // UI 线程调用：把排队中的请求交给 handler 处理。返回处理了几个。
    int Pump(const Handler& handler);

    // 停机时清空排队中还没被取走的请求，**并**唤醒所有正在等待的提交方
    // （让他们立刻拿到 false，不必干等到 waitMs 超时）。
    void Interrupt();

    // 排队中还没被取走的请求数（诊断与测试用）。
    size_t Pending() const;

    // 累计有多少个请求是**客户端超时走人之后**才被 UI 线程处理完的。
    // >0 说明有命令跑得比等待时限还长 —— 诊断"界面卡住"时用得上。
    size_t Abandoned() const;

private:
    struct Job {
        std::wstring request;
        std::wstring response;
        bool done = false;
        bool abandoned = false;   // 提交方已超时走人
    };
    using JobPtr = std::shared_ptr<Job>;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<JobPtr> queue_;
    bool interrupted_ = false;   // 停机：不再等结果
    size_t abandoned_ = 0;       // 累计被放弃的请求数
};

}  // namespace aiqueue
