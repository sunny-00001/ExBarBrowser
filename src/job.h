// 长任务作业服务：把"扫目录 / 查重 / 比较 / 算摘要"放到工作线程上跑，
// 并给出进度与取消。
//
// 【为什么需要它】这五条命令（tool.dupfinder / tool.hash / tool.compare /
// tool.diskusage / tool.topn）过去同步跑在 UI 线程上：一个几万文件的目录
// 能把界面冻住十几秒，用户连"取消"都按不了 —— UI 线程被占着，
// 连定时器都不转。预算（budgetMs）只保证"不会假死到没完"，
// 不解决"中途不能停"，也不解决"界面不冻结"。
//
// 【三条硬约束】
//   1. **工作线程产出，UI 线程取走**：共享一份作业记录 + 互斥锁。
//      调用方永远拿到的是**快照**，不会看到写了一半的中间态。
//   2. **取消是协作式的**：置一个原子位，数据层在循环里查。
//      已经被系统调用卡住的那一步没法中断 —— 这是诚实的边界，不是缺陷。
//   3. **完成与进度都可订阅**：作业结束/进度变化时回调钩子，
//      由上层转成事件推送（见 src/app_jobs.cpp），客户端不必轮询。
//
// 【为什么是进程级单例】作业必须在**命令返回之后**继续存在（异步模式下
// 命令立即返回，作业还在跑），所以它不能是调用栈上的局部对象。
// 这与 Config::Get() 是同一类东西：进程级服务状态。文件里不留裸全局变量，
// 状态一律收在 Service 实例里，由 Service::Get() 暴露。
#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json.h"
#include "scan_ctl.h"

namespace job {

// 作业体：在**工作线程**上跑。
// 填 out（结构化结果），返回 false = 失败（err 给人可读原因）。
// 随时可查 control.Cancelled() 提前收工。
using Body = std::function<bool(scanctl::Control& control, json::Value* out, std::wstring* err)>;

// 作业状态快照（值的拷贝，调用方可以长期持有）。
struct Status {
    std::wstring id;
    std::wstring kind;    // hash / dup / compare / usage / topn
    std::wstring target;  // 人可读描述（一般是路径）
    bool done = false;    // 工作线程已收工
    bool cancelled = false;
    bool failed = false;
    std::wstring error;   // 失败原因（failed 时）
    std::wstring stage;   // 当前阶段说明
    unsigned long long doneCount = 0;   // 已处理
    unsigned long long totalCount = 0;  // 总量（0 = 未知）
    json::Value result = json::Value::MakeObject();  // 成功时的结构化结果
};

class Service {
public:
    static Service& Get();
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // 启动一个长任务，**立即返回**作业 id（空串 = 启动失败）。
    // 工作线程在 body 返回后自行收工；调用方用 Get/WaitFinished 拿结果。
    std::wstring Start(const std::wstring& kind, const std::wstring& target, Body body);

    // 取作业快照。找不到返回 false。
    bool Get(const std::wstring& id, Status* out);

    // 请求取消（幂等）。返回 false = 没有这个 id，或它已经结束了。
    bool Cancel(const std::wstring& id);

    // 列出全部作业（新的在前）。
    std::vector<Status> List();

    // 进度钩子：每次进度变化回调一次（内部已按 200ms 节流）。
    // 由上层决定转成什么（目前是事件推送）。
    void SetProgressHook(std::function<void(const Status&)> fn);

    // 完成钩子：作业收工时回调一次（在工作线程上）。
    void SetDoneHook(std::function<void(const Status&)> fn);

    // 等作业收工并**等待工作线程真正退出**（把线程 join 掉）。
    // 返回 false = 超时还没完。timeoutMs = 0 表示无限等。
    //
    // 为什么必须 join：作业线程捕获了 Service 的地址，
    // 不 join 就让 Service 析构 = 工作线程往已析构对象上写 = 崩。
    bool WaitFinished(const std::wstring& id, unsigned timeoutMs);

    // 进程退出前调用：取消全部作业并 join 所有线程。
    void Shutdown();

private:
    struct Record;

    Service() = default;

    // 拷一份状态快照。调用方必须已持有 mu_（名字带 Locked 就是这个意思）。
    Status SnapshotLocked(const Record& rec) const;
    // join 掉作业的工作线程（幂等）。必须在**不持锁**时调用：
    // 工作线程收工时要拿同一把锁写状态。
    void JoinRecord(const std::shared_ptr<Record>& rec);

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Record>> jobs_;   // 新的在后
    std::function<void(const Status&)> progressHook_;
    std::function<void(const Status&)> doneHook_;
    unsigned long long nextId_ = 1;
};

}  // namespace job
