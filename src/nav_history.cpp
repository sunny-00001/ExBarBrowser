#include "nav_history.h"

namespace nav {

std::vector<std::wstring> History::backward() const {
    std::vector<std::wstring> out;
    for (int i = idx_ - 1; i >= 0; --i) out.push_back(hist_[static_cast<size_t>(i)]);
    return out;
}

std::vector<std::wstring> History::forward() const {
    std::vector<std::wstring> out;
    for (size_t i = static_cast<size_t>(idx_ + 1); i < hist_.size(); ++i) out.push_back(hist_[i]);
    return out;
}

void History::Reset(const std::wstring& path) {
    hist_.clear();
    idx_ = -1;
    pending_.Clear();
    if (!path.empty()) {
        hist_.push_back(path);
        idx_ = 0;
    }
}

void History::Push(const std::wstring& path) {
    // 截断"前进"分支：从中间导航后，原来的前进项已经不可达。
    // 少这一步的典型症状是"后退能退，但退回来的顺序是错的"。
    if (idx_ >= 0 && idx_ + 1 < static_cast<int>(hist_.size()))
        hist_.resize(static_cast<size_t>(idx_) + 1);
    hist_.push_back(path);
    idx_ = static_cast<int>(hist_.size()) - 1;
}

bool History::Request(const std::wstring& path, bool push) {
    const bool sameAsHere = here() == path;
    // 发起前先记下游标：失败要退回这里，否则"导航失败"会把游标留在半路。
    pending_.prevIdx = idx_;
    pending_.pushed = false;

    if (push && !sameAsHere) {
        Push(path);
        pending_.pushed = true;
    } else if (!push && sameAsHere) {
        // 原地替换：调用方要重新进入同一路径（例如强制刷新式重导航）。
        // 不改栈，只把 pending 指过来，让完成回调认出这是"同一次导航"。
    }
    pending_.path = path;
    return !(sameAsHere && push);
}

bool History::Goto(int target) {
    if (target < 0 || target >= static_cast<int>(hist_.size())) return false;
    const int old = idx_;
    idx_ = target;
    // 移动游标不改栈结构：pending.pushed=false 表示"失败时不需要删条目，
    // 只把游标退回原处"。
    pending_.path = hist_[static_cast<size_t>(target)];
    pending_.prevIdx = old;
    pending_.pushed = false;
    return true;
}

std::wstring History::Complete(const std::wstring& path) {
    if (path.empty()) {
        pending_.Clear();
        return here();
    }

    const bool hasIdx = idx_ >= 0 && idx_ < static_cast<int>(hist_.size());
    if (pending_.active()) {
        if (path == pending_.path) {
            // 同一次导航的完成回报：栈已由发起方写好。
        } else if (hasIdx && hist_[static_cast<size_t>(idx_)] == pending_.path) {
            // 重定向 / 规范化：就地收敛到真实路径。pending 跟着更新，
            // 这样多级重定向（文档 → OneDrive\文档 → 再跳）继续就地收敛，
            // 而不是每跳压一条。
            hist_[static_cast<size_t>(idx_)] = path;
            pending_.path = path;
        } else {
            // 发起方没写栈（push=false 的原地重进被重定向到别处）：正常压条。
            Push(path);
        }
    } else if (!hasIdx || hist_[static_cast<size_t>(idx_)] != path) {
        // 视图内部发起的导航（双击目录等，不经过 Request）：压新条。
        Push(path);
    }

    pending_.Clear();
    return here();
}

void History::Fail() {
    // 乐观压入的那条一定在末尾：直接弹掉即可，不需要按值查找。
    if (pending_.pushed) {
        if (!hist_.empty()) hist_.pop_back();
    }
    if (pending_.prevIdx >= 0) idx_ = pending_.prevIdx;
    // 走到这里说明栈可能已被别的路径动过（例如重定向后再失败）：
    // 游标必须重新夹紧，否则 canBack/canForward 会给出错误答案。
    ClampIdx();
    pending_.Clear();
}

void History::ClearKeepingHere() {
    // 保留"当前目录"作为唯一一条：清成空栈会让后退/前进状态、以及再次导航时
    // 的压栈基准都失去参照。当前目录本身不是"历史记录"。
    const std::wstring cur = here();  // 按值：下面要 clear
    pending_.Clear();
    hist_.clear();
    if (!cur.empty()) {
        hist_.push_back(cur);
        idx_ = 0;
    } else {
        idx_ = -1;
    }
}

void History::Restore(std::vector<std::wstring> hist, int idx) {
    hist_ = std::move(hist);
    idx_ = idx;
    pending_.Clear();
    ClampIdx();
}

void History::ClampIdx() {
    if (hist_.empty()) {
        idx_ = -1;
        return;
    }
    if (idx_ < 0) idx_ = 0;
    if (idx_ >= static_cast<int>(hist_.size())) idx_ = static_cast<int>(hist_.size()) - 1;
}

}  // namespace nav
