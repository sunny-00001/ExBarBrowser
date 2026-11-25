#include "tab_set.h"

#include <algorithm>

namespace core {

int TabSet::Add(const std::wstring& path, const std::wstring& title, int index) {
    TabRef t;
    t.id = nextId_++;
    t.path = path;
    t.title = title;
    if (index < 0 || index > static_cast<int>(items_.size())) index = static_cast<int>(items_.size());
    items_.insert(items_.begin() + index, t);
    mru_.insert(mru_.begin(), t.id);
    return t.id;
}

bool TabSet::Remove(int id) {
    auto it = std::find_if(items_.begin(), items_.end(), [id](const TabRef& t) { return t.id == id; });
    if (it == items_.end()) return false;
    items_.erase(it);
    mru_.erase(std::remove(mru_.begin(), mru_.end(), id), mru_.end());
    return true;
}

void TabSet::SetTitle(int id, const std::wstring& title) {
    for (auto& t : items_)
        if (t.id == id) {
            t.title = title;
            return;
        }
}

void TabSet::SetPath(int id, const std::wstring& path) {
    for (auto& t : items_)
        if (t.id == id) {
            t.path = path;
            return;
        }
}

void TabSet::SetLocked(int id, bool locked) {
    for (auto& t : items_)
        if (t.id == id) {
            t.locked = locked;
            return;
        }
}

void TabSet::SetAlias(int id, const std::wstring& alias) {
    for (auto& t : items_)
        if (t.id == id) {
            t.alias = alias;
            return;
        }
}

void TabSet::SetColor(int id, int color) {
    if (color < 0 || color >= kTabColorCount) color = 0;  // 越界=无颜色，不保留非法下标
    for (auto& t : items_)
        if (t.id == id) {
            t.color = color;
            return;
        }
}

// 重复判定按路径（大小写不敏感）：Windows 路径大小写不敏感，
// 而标签路径来自 Shell 回调，同一目录可能以不同大小写出现两次。
std::vector<int> TabSet::DuplicatesOf(int id) const {
    std::vector<int> out;
    const TabRef* self = Find(id);
    if (!self || self->path.empty()) return out;
    for (const auto& t : items_) {
        if (t.id == id) continue;
        if (str::IEquals(t.path, self->path)) out.push_back(t.id);
    }
    return out;
}

bool TabSet::HasDuplicates() const {
    // 复用与菜单同一个判定（core::PathRepeated），不另写一份两两比较 ——
    // 两份实现一旦分叉，就会出现"菜单说没有重复、这里说有"，两个都说"对"。
    std::vector<std::wstring> paths;
    paths.reserve(items_.size());
    for (const auto& t : items_) paths.push_back(t.path);
    for (size_t i = 0; i < paths.size(); ++i)
        if (PathRepeated(paths, i)) return true;
    return false;
}

int TabSet::IndexOf(int id) const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].id == id) return static_cast<int>(i);
    return -1;
}

int TabSet::IdAt(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return 0;
    return items_[static_cast<size_t>(index)].id;
}

const TabRef* TabSet::Find(int id) const {
    for (const auto& t : items_)
        if (t.id == id) return &t;
    return nullptr;
}

void TabSet::SetPinned(int id, bool pinned) {
    const int from = IndexOf(id);
    if (from < 0) return;
    if (items_[static_cast<size_t>(from)].pinned == pinned) return;
    items_[static_cast<size_t>(from)].pinned = pinned;
    // 固定区 = 开头的连续 pinned。
    //   pin   ：移到固定区末尾 → 最终下标 = 固定项总数 - 1。
    //   unpin ：移到第一个未固定位（Chrome 语义）→ 最终下标 = 剩余固定项数。
    // 用"总数"而不是"前缀长度"：待固定项此刻可能还在非固定区中间，
    // 前缀长度只数到第一个未固定项就停了。
    int total = 0;
    for (const auto& t : items_)
        if (t.pinned) ++total;
    Move(id, pinned ? total - 1 : total);
}

void TabSet::Move(int id, int toIndex) {
    int from = IndexOf(id);
    if (from < 0) return;
    if (toIndex < 0) toIndex = 0;
    if (toIndex >= static_cast<int>(items_.size())) toIndex = static_cast<int>(items_.size()) - 1;
    if (from == toIndex) return;
    TabRef t = items_[static_cast<size_t>(from)];
    items_.erase(items_.begin() + from);
    items_.insert(items_.begin() + toIndex, t);
}

void TabSet::Touch(int id) {
    if (IndexOf(id) < 0) return;
    mru_.erase(std::remove(mru_.begin(), mru_.end(), id), mru_.end());
    mru_.insert(mru_.begin(), id);
}

int TabSet::PrevMru(int currentId) const {
    if (mru_.size() < 2) return 0;
    if (mru_.empty() || mru_[0] != currentId) return mru_.empty() ? 0 : mru_[0];
    return mru_[1];
}

int TabSet::CycleMru(int dir) {
    if (items_.size() < 2) return 0;
    auto alive = [this](int id) { return IndexOf(id) >= 0; };
    if (!cycleActive_) {
        // 快照：先按实时 MRU，再补上尚未 Touch 过的标签（保持固定顺序）。
        cycleOrder_.clear();
        for (int id : mru_)
            if (alive(id)) cycleOrder_.push_back(id);
        for (const auto& t : items_)
            if (std::find(cycleOrder_.begin(), cycleOrder_.end(), t.id) == cycleOrder_.end())
                cycleOrder_.push_back(t.id);
        cycleActive_ = true;
        cyclePos_ = 0;   // mru_[0] 就是当前标签
    }
    // 循环期间可能有标签被关掉：先剔除失效 id，再取模，避免返回到已不存在的标签。
    cycleOrder_.erase(std::remove_if(cycleOrder_.begin(), cycleOrder_.end(),
                                     [&](int id) { return !alive(id); }),
                      cycleOrder_.end());
    if (cycleOrder_.empty()) return 0;
    const int n = static_cast<int>(cycleOrder_.size());
    cyclePos_ = ((cyclePos_ + dir) % n + n) % n;
    return cycleOrder_[static_cast<size_t>(cyclePos_)];
}

void TabSet::ResetMruCycle() {
    cycleActive_ = false;
    cycleOrder_.clear();
    cyclePos_ = 0;
}

void TabSet::PushClosed(const std::wstring& path) {
    if (path.empty()) return;
    closed_.erase(std::remove(closed_.begin(), closed_.end(), path), closed_.end());
    closed_.insert(closed_.begin(), path);
    const size_t limit = closedLimit_ > 0 ? static_cast<size_t>(closedLimit_) : 0;
    if (closed_.size() > limit) closed_.resize(limit);
}

bool TabSet::PopClosed(std::wstring* path) {
    if (closed_.empty()) return false;
    if (path) *path = closed_.front();
    closed_.erase(closed_.begin());
    return true;
}

bool TabSet::PopClosedAt(int index, std::wstring* path) {
    if (index < 0 || index >= static_cast<int>(closed_.size())) return false;
    if (path) *path = closed_[static_cast<size_t>(index)];
    closed_.erase(closed_.begin() + index);
    return true;
}

void TabSet::SetClosed(const std::vector<std::wstring>& list) {
    closed_ = list;
    const size_t limit = closedLimit_ > 0 ? static_cast<size_t>(closedLimit_) : 0;
    if (closed_.size() > limit) closed_.resize(limit);
}

void TabSet::SetClosedLimit(int n) {
    // 0/负数一律按"不允许历史"处理，而不是"不限" —— 后者会让配置写错时
    // settings.json 无限膨胀。上限 100 与配置侧的夹取一致。
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    closedLimit_ = n;
    // 收紧上限后立刻裁掉超出的部分：否则"改了设置但列表还那么长"，
    // 下次 PushClosed 才动，看起来像没生效。
    const size_t limit = static_cast<size_t>(closedLimit_);
    if (closed_.size() > limit) closed_.resize(limit);
}

void TabSet::Clear() {
    items_.clear();
    mru_.clear();
    closed_.clear();
    nextId_ = 1;
}

}  // namespace core
