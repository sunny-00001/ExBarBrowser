// 请求队列（aiqueue）契约自测。
//
// 【为什么这条单测存在】这块代码错过一个**内存被写坏**的坑：
// 早期实现把请求对象放在提交方的**栈上**，队列里存裸指针。于是：
//   提交方等待超时 → 返回 → 栈帧销毁
//   → UI 线程这时才处理完，往 job->response 写结果
//   = 写已释放的栈（提交方栈帧可能已被别的线程复用，写到谁头上全看运气）。
//
// 触发条件现实得很：任何跑得比等待时间长的命令都会走到这条路径
// （一次大目录的磁盘扫描、一次大树的复制）。
//
// 【这条单测能证明什么、不能证明什么】
//   · 能证明：超时后 UI 线程处理那个请求**不会崩**，队列之后仍然可用，
//     提交方拿到的结果符合约定。
//   · 不能证明：旧实现下"写已释放栈"一定当场崩 —— 未定义行为，
//     可能悄悄改掉别人的数据而不报错。
//     所以真正的防线是**结构上的**：请求对象改成共享所有权
//     （shared_ptr），这条路径压根不存在"对象没了"的可能。
//     单测守的是"别把结构改回去"。
#include "common.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "ai_queue.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 正常路径：提交方等到结果。
void TestNormal() {
    std::printf("正常路径\n");
    aiqueue::Queue q;
    std::wstring resp;
    bool got = false;

    std::thread submitter([&] { got = q.Submit(L"hello", 3000, &resp); });
    // UI 线程稍后泵一次（模拟定时器驱动）。
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int n = q.Pump([](const std::wstring& r) { return L"echo:" + r; });
    submitter.join();

    Check(got, "提交方拿到了结果");
    Check(n == 1, "Pump 处理了 1 个请求");
    Check(resp == L"echo:hello", "响应内容是 handler 的返回值");
    Check(q.Pending() == 0, "处理完队列为空");
}

// 关键路径：提交方超时走人，而 UI 线程**随后**才处理那个请求。
// 这正是旧实现写已释放栈的那条路径。
void TestTimeoutThenLateComplete() {
    std::printf("超时后 UI 线程仍处理那个请求（旧的写已释放栈场景）\n");
    aiqueue::Queue q;
    bool got = true;
    std::wstring resp = L"非空";

    // 等待 60ms 就超时；UI 线程的 handler 要跑 300ms ——
    // 保证 handler 一定在提交方放弃之后才写回结果。
    std::thread submitter([&] { got = q.Submit(L"slow", 60, &resp); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int n = q.Pump([](const std::wstring& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return L"late:" + r;
    });
    submitter.join();

    Check(!got, "超时：提交方没有拿到结果");
    Check(resp.empty(), "超时：响应被清空（不会拿到半截结果）");
    Check(n == 1, "UI 线程仍然安全地把那个请求处理完了");
    Check(q.Pending() == 0, "处理完队列为空");

    // 超时之后队列还得能用 —— 上一条请求的"残骸"不能污染后面的。
    std::wstring resp2;
    bool got2 = false;
    std::thread submitter2([&] { got2 = q.Submit(L"again", 3000, &resp2); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.Pump([](const std::wstring& r) { return L"ok:" + r; });
    submitter2.join();
    Check(got2 && resp2 == L"ok:again", "超时之后队列仍然可用");
}

// 停机打断：正在等待的提交方立刻拿到 false，不必干等到超时。
void TestInterrupt() {
    std::printf("停机打断等待中的提交方\n");
    aiqueue::Queue q;
    bool got = true;
    std::thread submitter([&] { got = q.Submit(L"pending", 5000, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const auto t0 = std::chrono::steady_clock::now();
    q.Interrupt();
    submitter.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    Check(!got, "被打断的提交方拿到 false");
    Check(ms < 2000, "被打断的提交方没有干等到 5 秒超时");
    Check(q.Pending() == 0, "打断后排队清空");
}

// 空 handler：不能崩（防御性 —— 队列不该依赖 handler 非空）。
void TestNullHandler() {
    std::printf("空 handler 不崩\n");
    aiqueue::Queue q;
    bool got = false;
    std::thread submitter([&] { got = q.Submit(L"x", 3000, nullptr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int n = q.Pump(nullptr);
    submitter.join();
    Check(n == 1, "空 handler 也算处理了 1 个");
    Check(got, "提交方拿到（空）结果而不是卡死");
}

}  // namespace

int main() {
    TestNormal();
    TestTimeoutThenLateComplete();
    TestInterrupt();
    TestNullHandler();

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
