// 标签集合模型（零 Win32）：顺序 / 锁定 / MRU / 关闭历史。
//
// 视图（ShellView）不在本类里：App 用 id 关联，模型只回答"顺序是什么、谁是上一个"。
#pragma once
#include <string>
#include <vector>

namespace core {

struct TabRef {
    int id = 0;
    std::wstring path;
    std::wstring title;
    bool locked = false;
};

class TabSet {
public:
    int Add(const std::wstring& path, const std::wstring& title, int index = -1);
    bool Remove(int id);

    void SetTitle(int id, const std::wstring& title);
    void SetPath(int id, const std::wstring& path);
    void SetLocked(int id, bool locked);

    const std::vector<TabRef>& items() const { return items_; }
    int Count() const { return static_cast<int>(items_.size()); }
    int IndexOf(int id) const;
    int IdAt(int index) const;
    const TabRef* Find(int id) const;

    void Move(int id, int toIndex);
    void Touch(int id);                 // MRU 置顶
    int PrevMru(int currentId) const;   // 上一个激活的标签 id（无则 0）

    void PushClosed(const std::wstring& path);
    bool PopClosed(std::wstring* path);
    const std::vector<std::wstring>& closed() const { return closed_; }
    void SetClosed(const std::vector<std::wstring>& list);

    void Clear();

private:
    std::vector<TabRef> items_;
    std::vector<int> mru_;
    std::vector<std::wstring> closed_;
    int nextId_ = 1;
};

}  // namespace core
