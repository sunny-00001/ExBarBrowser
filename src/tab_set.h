// 标签集合模型（零 Win32）：顺序 / 锁定 / MRU / 关闭历史 / 用户改名与颜色。
//
// 视图（ShellView）不在本类里：App 用 id 关联，模型只回答"顺序是什么、谁是上一个"。
#pragma once
#include <string>
#include <vector>

#include "nav_path.h"   // nav::Leaf：纯字符串语义，不含 Win32（R2 仍成立）
#include "str.h"        // str::IEquals：大小写不敏感比较的唯一实现

namespace core {

// 标签颜色的档数（含 0 = 无颜色）。**唯一定义**。
//
// 为什么放在 core 而不是 Theme：这是"一个标签能有几种颜色"的领域事实，
// 而 Theme 只负责"第 i 档显示成什么 RGB"。定义在 Theme 里会逼出下面的写法 ——
// 校验层（tab_set.cpp 的 SetColor、config.cpp 的读盘夹取）永远拿不到 Theme，
// 只能各自写死 `color > 7`。三处硬编码一旦与色板长度脱钩，
// 表现就是"选了第 8 档，重启后变回无颜色"，且没有任何报错。
inline constexpr int kTabColorCount = 8;

struct TabRef {
    int id = 0;
    std::wstring path;
    std::wstring title;   // Shell 给的显示名（随导航变化）
    // 用户重命名。空 = 跟随 path/title。单独一个字段而不是直接改 title：
    // OnTabNav 每次导航都会 SetTitle(系统名)，直接改 title 会在下次刷新时被冲掉
    // （表现为"改完名字进一次目录就变回去了"）。
    std::wstring alias;
    // 标签颜色下标（0 = 无颜色）。存下标不存 COLORREF：
    // 本文件零 Win32，且颜色必须与深浅主题配套换算（见 Theme::TabColorFill）。
    int color = 0;
    bool locked = false;
};

// 标签最终显示名：用户改名 > Shell 名 > 路径末级。
// 唯一实现 —— 标签栏、窗口标题、命令面板都读这里，否则会出现
// "标签栏是改名后的、窗口标题还是旧名"。
inline std::wstring DisplayTitle(const TabRef& t) {
    if (!t.alias.empty()) return t.alias;
    if (!t.title.empty()) return t.title;
    return nav::Leaf(t.path);
}

// 一条路径在一组路径里是否出现两次以上（大小写不敏感）。
//
// 为什么单独抽出来：标签右键菜单要判断"关闭重复"能不能点，而它手里只有
// TabStripItem（没有 TabRef、也不该反向依赖 TabSet）。判定口径必须是同一份 ——
// 否则会出现"菜单亮着、点了却一个也没关掉"（那是很难解释的交互）。
// 比较原语统一走 str::IEquals（Windows 路径语义 + Shell 回调可能给不同大小写）。
inline bool PathRepeated(const std::vector<std::wstring>& paths, size_t self) {
    if (self >= paths.size()) return false;
    // 空路径不算重复：新建失败/尚未落定的标签路径是空的，两条空路径
    // 会被当成"同目录"，于是"关闭重复"把它们互相关掉。
    if (paths[self].empty()) return false;
    for (size_t k = 0; k < paths.size(); ++k) {
        if (k == self || paths[k].empty()) continue;
        if (str::IEquals(paths[k], paths[self])) return true;
    }
    return false;
}

class TabSet {
public:
    int Add(const std::wstring& path, const std::wstring& title, int index = -1);
    bool Remove(int id);

    void SetTitle(int id, const std::wstring& title);
    void SetPath(int id, const std::wstring& path);
    void SetLocked(int id, bool locked);
    // 空串 = 取消改名（回到跟随路径）。
    void SetAlias(int id, const std::wstring& alias);
    // 0..Theme::kTabColorCount-1；越界视为 0（无颜色）。
    void SetColor(int id, int color);

    const std::vector<TabRef>& items() const { return items_; }
    int Count() const { return static_cast<int>(items_.size()); }
    int IndexOf(int id) const;
    int IdAt(int index) const;
    const TabRef* Find(int id) const;
    // 与 id 同路径的**其他**标签（大小写不敏感，"关闭重复标签页"用）。
    std::vector<int> DuplicatesOf(int id) const;
    // 当前集合里是否存在重复路径（菜单项能不能点）。
    bool HasDuplicates() const;

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
