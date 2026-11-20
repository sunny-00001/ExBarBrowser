#include "tab_set.h"

#include <algorithm>
#include <cwctype>

namespace {

// 路径比较按大小写不敏感（Windows 路径语义）。这里不用 _wcsicmp：
// 本文件守 R2（零 Win32，可在任意平台单测），CRT 的 _wcsicmp 是 MSVC 方言；
// towlower 是标准库，行为一致且足够（只比较相等性）。
bool IPathEqual(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

}  // namespace

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
    if (color < 0 || color > 7) color = 0;   // 越界=无颜色，不保留非法下标
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
        if (IPathEqual(t.path, self->path)) out.push_back(t.id);
    }
    return out;
}

bool TabSet::HasDuplicates() const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].path.empty()) continue;
        for (size_t k = i + 1; k < items_.size(); ++k) {
            if (IPathEqual(items_[i].path, items_[k].path)) return true;
        }
    }
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

void TabSet::PushClosed(const std::wstring& path) {
    if (path.empty()) return;
    closed_.erase(std::remove(closed_.begin(), closed_.end(), path), closed_.end());
    closed_.insert(closed_.begin(), path);
    if (closed_.size() > 20) closed_.resize(20);
}

bool TabSet::PopClosed(std::wstring* path) {
    if (closed_.empty()) return false;
    if (path) *path = closed_.front();
    closed_.erase(closed_.begin());
    return true;
}

void TabSet::SetClosed(const std::vector<std::wstring>& list) {
    closed_ = list;
    if (closed_.size() > 20) closed_.resize(20);
}

void TabSet::Clear() {
    items_.clear();
    mru_.clear();
    closed_.clear();
    nextId_ = 1;
}

}  // namespace core
