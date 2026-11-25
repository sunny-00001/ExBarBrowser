// 导航历史栈：全项目唯一实现（每个标签一份）。
//
// 为什么必须抽成组件：
//   这段逻辑以前散在 App::NavigateActive / OnTabNav / OnTabNavFailed / GotoHistory
//   四个函数里，各自手写 `hist.resize(idx+1)` + `push_back` + `idx = size()-1`。
//   同一套"压栈"写了 4 遍，规则一旦分叉就会出现：后退要按两次、导航失败后
//   历史里留死条目、重定向留下多条记录。这类 bug 的共同点是**症状远离现场**
//   （用户看到的是"后退不对"，根因在别处的 push_back），所以必须收口。
//
// 边界：本类只管"路径怎么进栈、游标怎么动、失败怎么回滚"。
// 不碰 Shell、不碰窗口 —— 真正的导航由调用方拿 here() 去执行。
#pragma once
#include <string>
#include <vector>

namespace nav {

class History {
public:
    // 每窗格历史栈的默认上限。实际深度由外部（App 从设置 historyDepth）注入：
    // 本类是纯逻辑组件，不读 Config —— 单测单独编本 TU 时不应拖入配置层。
    // 无上限时 browsing 一天就会让配置和内存一起膨胀。
    static constexpr size_t kDefaultMaxEntries = 512;

    explicit History(size_t maxEntries = kDefaultMaxEntries) : maxEntries_(maxEntries) {}

    // 运行中改深度（config.set historyDepth）：立即把已有栈裁到新上限。
    // 传 0 视为默认值，防止手改配置写出 0 让历史永远空着。
    void SetMaxEntries(size_t maxEntries) {
        maxEntries_ = maxEntries ? maxEntries : kDefaultMaxEntries;
        const size_t drop = Trim();
        if (drop) idx_ -= static_cast<int>(drop);   // 裁剪后原游标整体左移
        ClampIdx();
    }
    size_t maxEntries() const { return maxEntries_; }

    // 一次导航的待确认状态。
    //
    // 为什么需要它：Shell 的导航是异步的，且可能**重定向**（文档 → OneDrive\文档）
    // 或规范化（尾斜杠/大小写）。发起时我们只知道"请求的目标"，完成时才知道
    // "真实落到哪"。pending 把两者对上，避免一次导航留下两条历史。
    struct Pending {
        std::wstring path;    // 本次显式请求的目标
        int prevIdx = -1;     // 发起前的游标（失败时退回这里）
        bool pushed = false;  // 本次是否新压了一条（失败时要删掉它）
        bool active() const { return !path.empty(); }
        void Clear() { *this = Pending{}; }
    };

    // ---- 读 ----
    bool empty() const { return hist_.empty(); }
    size_t size() const { return hist_.size(); }
    int idx() const { return idx_; }
    // 当前路径。空栈或游标越界返回空串（不返回 hist_[0] 之类的猜测值）。
    const std::wstring& here() const {
        static const std::wstring kEmpty;
        return (idx_ >= 0 && idx_ < static_cast<int>(hist_.size())) ? hist_[static_cast<size_t>(idx_)]
                                                                    : kEmpty;
    }
    const std::wstring& at(size_t i) const { return hist_[i]; }
    // 会话持久化用：完整历史栈（只读）。恢复走 Restore。
    const std::vector<std::wstring>& entries() const { return hist_; }

    bool canBack() const { return idx_ > 0; }
    bool canForward() const { return idx_ >= 0 && idx_ + 1 < static_cast<int>(hist_.size()); }
    int backCount() const { return idx_ > 0 ? idx_ : 0; }
    int forwardCount() const { return static_cast<int>(hist_.size()) - idx_ - 1; }

    // 后退/前进列表（导航栏下拉用）。顺序是"离当前由近到远"。
    std::vector<std::wstring> backward() const;
    std::vector<std::wstring> forward() const;

    const Pending& pending() const { return pending_; }

    // ---- 写（三个动作，覆盖全部场景）----

    // 新建标签：把初始路径作为唯一一条。
    void Reset(const std::wstring& path);

    // 用户/命令发起的导航：压新条并记下 pending。
    // 返回 false = 目标就是当前路径且要求不压栈，调用方可以跳过真正的导航。
    //   push=false  → 原地替换当前条（用于重定向收敛后的重进）
    //   push=true   → 截断"前进"分支后压新条
    bool Request(const std::wstring& path, bool push);

    // 跳到历史里的某一条（后退/前进/跳转）。越界返回 false。
    bool Goto(int target);

    // Shell 回报导航完成。path 是**真实**落到的路径（可能被重定向/规范化）。
    // 返回需要调用方同步的当前路径（已就地收敛到真实路径）。
    std::wstring Complete(const std::wstring& path);

    // 导航失败：撤销 pending 带来的栈变化，游标退回。
    void Fail();

    // 只剩当前路径一条（"清除历史记录"）。
    void ClearKeepingHere();

    // 供序列化/恢复用：直接装填。
    void Restore(std::vector<std::wstring> hist, int idx);

private:
    // 压栈的唯一实现：截断前进分支 + push + 游标移到末尾。
    // 以前这段在 4 个地方各写一遍，是"后退要按两次"的根源。
    void Push(const std::wstring& path);

    void ClampIdx();
    // 把栈裁到 maxEntries_，返回从头部丢弃了几条。**不碰 idx_** ——
    // 调用方按各自语义调整游标（Push 落在末尾、SetMaxEntries/Restore 左移 drop）。
    // 裁剪口径只有这一份，Push 与 Restore/SetMaxEntries 都走它。
    size_t Trim();

    std::vector<std::wstring> hist_;
    int idx_ = -1;
    Pending pending_;
    size_t maxEntries_ = kDefaultMaxEntries;
};

}  // namespace nav
