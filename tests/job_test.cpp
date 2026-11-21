// 长任务作业服务自测（job::Service）。
//
// 【为什么这层必须测】作业服务是"命令返回之后还在跑"的那一段逻辑，
// 它出问题时的症状都很隐蔽：
//   1. 工作线程写状态与读取快照之间的竞态（读到写了一半的结果）；
//   2. 取消被当成失败（调用方会以为出了错而重试）；
//   3. 完成通知早于状态落库（订阅者收到"做完了"却查不到结果）；
//   4. 退出时没 join（工作线程往已析构的 Service 上写 —— 崩在关窗口那一刻）。
#include "common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "job.h"
#include "scan_ctl.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 作业体：立刻成功，结果里放一个可核对的数。
job::Body OkBody() {
    return [](scanctl::Control&, json::Value* out, std::wstring*) {
        out->Set(L"answer", json::Value::MakeInt(42));
        out->Set(L"label", json::Value::MakeStr(L"ok"));
        return true;
    };
}

}  // namespace

int main() {
    job::Service& svc = job::Service::Get();

    // ---------------------------------------------------------------
    std::printf("正常路径\n");
    {
        const std::wstring id = svc.Start(L"hash", L"C:\\tmp\\a.bin", OkBody());
        Check(!id.empty(), "拿到作业 id");
        Check(svc.WaitFinished(id, 5000), "等到了作业收工");

        job::Status st;
        Check(svc.Get(id, &st), "能查到作业");
        Check(st.done, "已收工");
        Check(!st.failed, "没有失败");
        Check(!st.cancelled, "没有被取消");
        Check(st.error.empty(), "没有错误信息");
        Check(st.kind == L"hash", "kind 正确");
        Check(st.target == L"C:\\tmp\\a.bin", "target 正确");
        Check(st.result.Find(L"answer") != nullptr, "结果里有 answer");
        if (st.result.Find(L"answer")) {
            Check(st.result.GetInt(L"answer", 0) == 42, "answer = 42");
        }
    }

    // ---------------------------------------------------------------
    std::printf("\n失败路径\n");
    {
        job::Body body = [](scanctl::Control&, json::Value*, std::wstring* err) {
            if (err) *err = L"故意的失败";
            return false;
        };
        const std::wstring id = svc.Start(L"dup", L"C:\\tmp", body);
        svc.WaitFinished(id, 5000);
        job::Status st;
        svc.Get(id, &st);
        Check(st.done, "已收工（失败也是收工）");
        Check(st.failed, "标记了 failed");
        Check(!st.cancelled, "失败**不是**取消（两件事不能混）");
        Check(st.error == L"故意的失败", "错误原因如实带回");
        Check(st.result.Find(L"answer") == nullptr, "失败没有结果");
    }

    // ---------------------------------------------------------------
    std::printf("\n取消路径\n");
    {
        std::atomic<bool> sawCancel{false};
        std::atomic<int> spins{0};
        job::Body body = [&sawCancel, &spins](scanctl::Control& ctl, json::Value* out,
                                              std::wstring*) {
            // 一直转到被叫停为止：模拟一个真正耗时的活。
            while (!ctl.Cancelled()) {
                spins.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            sawCancel.store(true);
            out->Set(L"stoppedAt", json::Value::MakeInt(spins.load()));
            return true;
        };
        const std::wstring id = svc.Start(L"usage", L"C:\\big", body);
        Check(!id.empty(), "拿到作业 id");

        // 等它真的转起来再取消：取消一个还没起步的作业也该有效，
        // 但那样测不到"作业体里查到了取消位"这一段。
        for (int i = 0; i < 200 && spins.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        Check(svc.Cancel(id), "取消请求被受理");
        Check(svc.WaitFinished(id, 5000), "作业随后收工");

        job::Status st;
        svc.Get(id, &st);
        Check(st.done, "已收工");
        Check(sawCancel.load(), "作业体里查到了取消位");
        Check(st.cancelled, "标记了 cancelled");
        Check(!st.failed, "被取消**不是**失败（调用方不该因此重试）");
        Check(st.result.Find(L"stoppedAt") != nullptr, "取消前算出的东西仍在结果里");
    }

    // ---------------------------------------------------------------
    std::printf("\n取消已结束的作业\n");
    {
        const std::wstring id = svc.Start(L"hash", L"x", OkBody());
        svc.WaitFinished(id, 5000);
        Check(!svc.Cancel(id), "对已结束的作业取消会失败（而不是假装成功）");
    }

    // ---------------------------------------------------------------
    std::printf("\n进度快照\n");
    {
        job::Body body = [](scanctl::Control& ctl, json::Value* out, std::wstring*) {
            ctl.Report(L"正在统计大小", 3, 10);
            ctl.Report(L"正在统计大小", 7, 10);
            out->Set(L"n", json::Value::MakeInt(10));
            return true;
        };
        const std::wstring id = svc.Start(L"usage", L"C:\\big", body);
        svc.WaitFinished(id, 5000);
        job::Status st;
        svc.Get(id, &st);
        // 进度写的是**记录**，不是只有回调才写 —— 所以最后一次进度一定在快照里。
        Check(st.stage == L"正在统计大小", "快照里有阶段说明");
        Check(st.doneCount == 7, "快照里有已处理数（7）");
        Check(st.totalCount == 10, "快照里有总量（10）");
    }

    // ---------------------------------------------------------------
    std::printf("\n进度回调\n");
    {
        std::atomic<int> pushes{0};
        svc.SetProgressHook([&pushes](const job::Status&) { pushes.fetch_add(1); });
        job::Body body = [](scanctl::Control& ctl, json::Value*, std::wstring*) {
            for (int i = 0; i < 5; ++i) {
                ctl.Report(L"步", static_cast<unsigned long long>(i), 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return true;
        };
        const std::wstring id = svc.Start(L"usage", L"C:\\big", body);
        svc.WaitFinished(id, 5000);
        Check(pushes.load() >= 1, "进度回调至少被调过一次");
        svc.SetProgressHook(nullptr);
    }

    // ---------------------------------------------------------------
    std::printf("\n完成回调\n");
    {
        std::atomic<int> doneHits{0};
        bool gotResult = false;
        svc.SetDoneHook([&doneHits, &gotResult](const job::Status& st) {
            doneHits.fetch_add(1);
            // 完成事件必须**带结果**：否则订阅者收到"做完了"还要再查一次，
            // 那等于把轮询换成了两段式轮询。
            gotResult = st.result.Find(L"answer") != nullptr;
        });
        const std::wstring id = svc.Start(L"hash", L"x", OkBody());
        svc.WaitFinished(id, 5000);
        Check(doneHits.load() == 1, "完成回调恰好一次");
        Check(gotResult, "完成事件里带着结果");
        svc.SetDoneHook(nullptr);
    }

    // ---------------------------------------------------------------
    std::printf("\n查不到与列清单\n");
    {
        job::Status st;
        Check(!svc.Get(L"job-does-not-exist", &st), "查不存在的 id 返回 false");

        const std::wstring a = svc.Start(L"hash", L"first", OkBody());
        const std::wstring b = svc.Start(L"hash", L"second", OkBody());
        svc.WaitFinished(a, 5000);
        svc.WaitFinished(b, 5000);
        const auto list = svc.List();
        Check(!list.empty(), "清单非空");
        Check(list.front().id == b, "清单里新的在前");
        bool sawA = false;
        for (const auto& s : list)
            if (s.id == a) sawA = true;
        Check(sawA, "清单里也有较早的作业");
    }

    // ---------------------------------------------------------------
    std::printf("\n等待超时\n");
    {
        std::atomic<bool> release{false};
        job::Body body = [&release](scanctl::Control&, json::Value*, std::wstring*) {
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return true;
        };
        const std::wstring id = svc.Start(L"usage", L"slow", body);
        Check(!svc.WaitFinished(id, 50), "等不到就返回 false（不装作等到）");
        release.store(true);
        Check(svc.WaitFinished(id, 5000), "作业最终能等到");
        svc.WaitFinished(id, 5000);   // 幂等：再等一次不该出事
        Check(true, "重复等待是安全的");
    }

    // ---------------------------------------------------------------
    std::printf("\n预算约定\n");
    {
        // "0 = 不限"只有一处定义（scan_ctl.h），数据层各自翻译 ——
        // 这里钉住的是那条约定本身。
        Check(scanctl::IsUnlimited(scanctl::kUnlimitedBudget), "0 被认作不限时");
        Check(!scanctl::IsUnlimited(1500), "正数不被认作不限时");
        Check(!scanctl::IsUnlimited(30000), "上界也不被认作不限时");
    }

    // ---------------------------------------------------------------
    std::printf("\n退出收尾\n");
    {
        std::atomic<bool> sawCancel{false};
        job::Body body = [&sawCancel](scanctl::Control& ctl, json::Value*, std::wstring*) {
            while (!ctl.Cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            sawCancel.store(true);
            return true;
        };
        const std::wstring id = svc.Start(L"usage", L"forever", body);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        // Shutdown 会取消全部并 join —— 不 join 的话工作线程会往已析构的
        // Service 上写，崩在退出那一刻，而且崩得莫名其妙。
        svc.Shutdown();
        Check(sawCancel.load(), "Shutdown 取消了在跑的作业");
        job::Status st;
        svc.Get(id, &st);
        Check(st.done, "Shutdown 之后作业已收工");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
