// 导航栏的交互动作：地址编辑 / Tab 补全 / 自绘下拉菜单
// （从 nav_bar.cpp 拆出，保持单文件 <=500 行）
#include "nav_bar.h"
#include "str.h"

#include "folder_tree.h"
#include "icon_resolve.h"
#include "popup_list.h"
#include "view_modes.h"

namespace {

// "此电脑"的 CLSID 不再在这里重复定义：唯一来源是 nav_path.h 的 nav::kThisPc。

// 统一走 ico::Resolve：特殊命名空间（此电脑/回收站/侧边栏/云存储）拿准确图标，
// 不再各自裸调 SHGetFileInfoW。
HICON IconFor(const std::wstring& path) {
    return ico::Resolve(path, true);
}

void FreeRows(std::vector<PopupRow>& rows) {
    for (auto& r : rows)
        if (r.icon) DestroyIcon(r.icon);
    rows.clear();
}

// 视图档位表在 view_modes.h（唯一来源）。这里原来有一份逐字相同的副本，
// 而两份表的**下标**同时被菜单回传与配置持久化使用 —— 改一处忘另一处就是
// "菜单显示的和实际切的不一致"，且无任何报错。
inline constexpr const viewmode::ViewMode* kViews = viewmode::kModes;
constexpr int kViewCount = viewmode::kCount;

}  // namespace

void NavBar::BeginEdit() {
    if (!edit_ || editing_) return;
    editing_ = true;
    SetWindowTextW(edit_, model_.AddressText().c_str());
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void NavBar::EndEdit(bool refocus) {
    if (!editing_) return;
    editing_ = false;
    tabMatches_.clear();
    tabIdx_ = -1;
    tabBase_.clear();
    if (refocus) {
        HWND main = hwnd_ ? GetAncestor(hwnd_, GA_ROOT) : nullptr;
        if (refocus_) refocus_();
        else if (main) SetFocus(main);  // 没接回调时至少别把焦点留在隐藏的 EDIT 上
    }
    if (edit_) ShowWindow(edit_, SW_HIDE);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring NavBar::AddressOfEdit() const {
    if (!edit_) return std::wstring();
    int n = GetWindowTextLengthW(edit_);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit_, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

void NavBar::CommitEdit() {
    if (!editing_) return;
    std::wstring text = AddressOfEdit();
    EndEdit(true);  // 先还焦点：后续导航/弹框都不该让键盘停在 EDIT 上
    if (text.empty() || text == model_.AddressText()) return;
    if (onNavigate_) onNavigate_(text);
}

bool NavBar::DoComplete() {
    if (!edit_) return false;
    std::wstring text = AddressOfEdit();
    if (text != tabBase_) {
        tabBase_ = text;
        tabMatches_.clear();
        tabIdx_ = -1;

        std::wstring parent = NavModel::ParentOf(text);
        std::wstring prefix = NavModel::LeafOfPath(text);
        if (parent.empty()) {
            parent = model_.path();
            prefix = text;
        }
        std::vector<std::pair<std::wstring, std::wstring>> kids;
        NavModel::ChildFolders(parent, kids);
        for (const auto& kv : kids) {
            if (prefix.empty() || str::IStartsWith(kv.first, prefix))
                tabMatches_.push_back(kv.first);
        }
        if (tabMatches_.empty()) return false;
    }
    if (tabMatches_.empty()) return false;

    tabIdx_ = (tabIdx_ + 1) % static_cast<int>(tabMatches_.size());
    std::wstring parent = NavModel::ParentOf(tabBase_);
    if (parent.empty()) parent = model_.path();
    std::wstring out = parent;
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    out += tabMatches_[tabIdx_];

    SetWindowTextW(edit_, out.c_str());
    int len = static_cast<int>(out.size());
    SendMessageW(edit_, EM_SETSEL, len, len);
    return true;
}

void NavBar::ShowChildMenu(int partIndex, POINT screen) {
    (void)screen;
    const auto& parts = model_.parts();
    if (partIndex < 0 || partIndex >= static_cast<int>(parts.size())) return;

    // 分隔符归属于它**左边**那一级：列该级自己的直接子文件夹。
    // 例：D:\Windhawk 右侧的 › 列 Windhawk 的内容，而不是 D:\ 下的同级目录
    // （与 mod ShowCrumbMenu 的 EnumChildFoldersAt(crumbPidls[partIndex]) 同口径）。
    const std::wstring target = parts[partIndex].path;
    if (target.empty()) return;

    // 顶层 = 该段的直接子文件夹；子节点由弹层按需懒加载、就地内联展开
    // （枚举与展开都走 folder_tree，与书签栏图标点击同一份实现）
    std::vector<TreeNode> top = folder_tree::Children(target);
    if (top.empty()) return;

    // 对齐到该段的 chevron 区（不是鼠标点），否则菜单会"飘"在鼠标位置
    int k = partIndex - firstShown_;
    RECT anchor = (k >= 0 && k < static_cast<int>(chevRects_.size())) ? chevRects_[k] : fieldRc_;
    POINT at{anchor.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);

    // 分隔符切到展开态「⌄」并立即重绘（模态菜单弹出前，用户能看到形态变化）
    openChev_ = partIndex;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
    // 点击 folder 行 chevron 内联展开子树（与 exbar.cpp treeMode 一致），
    // 而不是向右弹出新菜单；整棵树的 icon 由 ShowFolderTree 统一销毁。
    std::wstring picked = folder_tree::Show(hwnd_, *theme_, at, std::move(top));
    // 与 mod 28756-28758 一致：恢复 › 的同时清掉悬停态，避免光标下高亮残留
    openChev_ = -1;
    hotChev_ = -1;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
    if (!picked.empty() && onNavigate_) onNavigate_(picked);
}

void NavBar::ShowFoldMenu(POINT screen) {
    (void)screen;
    std::vector<TreeNode> top;

    if (firstShown_ > 0) {
        const auto& parts = model_.parts();
        for (int i = 0; i < firstShown_ && i < static_cast<int>(parts.size()); ++i)
            top.push_back({parts[i].label, parts[i].path, IconFor(parts[i].path),
                           0, -1, {}, true, false, false});
    } else {
        // 没折叠时这个钮给"此电脑"的顶层（驱动器/用户文件夹），与 mod 一致
        top = folder_tree::Children(nav::kThisPc);
    }
    if (top.empty()) return;

    POINT at{moreRc_.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);
    // 折叠菜单同样走内联树：点 chevron 就地展开，整棵树 icon 由弹层销毁
    std::wstring picked = folder_tree::Show(hwnd_, *theme_, at, std::move(top));
    if (!picked.empty() && onNavigate_) onNavigate_(picked);
}

void NavBar::ShowHistoryMenu(bool backward, POINT screen) {
    (void)screen;
    if (!historyList_) return;
    std::vector<std::wstring> items = historyList_(backward);
    if (items.empty()) return;

    std::vector<PopupRow> rows;
    int n = std::min<int>(static_cast<int>(items.size()), 24);
    for (int i = 0; i < n; ++i)
        rows.push_back({items[static_cast<size_t>(i)], items[static_cast<size_t>(i)],
                        IconFor(items[static_cast<size_t>(i)]), false, false, false, i});

    // 对齐到对应的分裂按钮（后退/前进），与资源管理器一致
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == (backward ? kBack : kForward)) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    int pick = ShowPopupList(hwnd_, *theme_, at, rows, 0);
    if (pick >= 0 && historyJump_) historyJump_(pick, backward);
    FreeRows(rows);
}

void NavBar::ShowDropMenu(POINT screen) {
    (void)screen;
    if (!historyList_) return;
    std::vector<std::wstring> back = historyList_(true);
    std::vector<std::wstring> fwd = historyList_(false);
    std::vector<std::wstring> recent;
    if (recentList_) recent = recentList_();
    if (back.empty() && fwd.empty() && recent.empty()) return;

    std::vector<PopupRow> rows;
    for (size_t i = 0; i < back.size() && i < 16; ++i)
        rows.push_back({back[i], back[i], IconFor(back[i]), false, false, false,
                        static_cast<int>(i)});
    if (!back.empty() && !fwd.empty())
        rows.push_back({L"", L"", nullptr, false, true, false, -1});
    for (size_t i = 0; i < fwd.size() && i < 16; ++i)
        rows.push_back({fwd[i], fwd[i], IconFor(fwd[i]), false, false, false,
                        1000 + static_cast<int>(i)});
    if (!recent.empty()) {
        if (!back.empty() || !fwd.empty()) rows.push_back({L"", L"", nullptr, false, true, false, -1});
        int n = 0;
        for (const auto& p : recent) {
            if (n >= 12) break;
            bool dup = false;
            for (const auto& r : rows)
                if (!r.separator && str::IEquals(r.path, p)) { dup = true; break; }
            if (!dup) {
                rows.push_back({p, p, IconFor(p), false, false, false, 2000 + n});
                ++n;
            }
        }
    }

    // 底部固定一条"清除历史记录"：列表里只要有可清的东西（后退/前进历史，
    // 或"最近访问"）就出现；三者全空时上面早就 return 了，这里不用再判。
    // 放在最后 + 前面加分隔线：它是"作用于整个列表"的动作，不该混在路径条目里。
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    rows.push_back({L"清除历史记录", L"", nullptr, false, false, false, 3000});

    // 与地址栏左对齐、从字段底边展开，宽度不小于字段宽
    POINT at{fieldRc_.left, fieldRc_.bottom};
    ClientToScreen(hwnd_, &at);
    // 地址框右侧 ⌄：严格与地址框同宽（左右边缘齐平），长路径靠省略号收
    LogDbg(L"[drop] field=%d..%d w=%d anchor=%d,%d", fieldRc_.left, fieldRc_.right,
           fieldRc_.right - fieldRc_.left, at.x, at.y);
    int pick = ShowPopupList(hwnd_, *theme_, at, rows, fieldRc_.right - fieldRc_.left, true);
    if (pick >= 0 && pick < static_cast<int>(rows.size())) {
        int data = rows[static_cast<size_t>(pick)].data;
        if (data >= 3000) {
            // 清空历史。不导航：用户只是整理了历史，当前目录不该跟着跳。
            if (clearHistory_) clearHistory_();
        } else if (data >= 2000) {
            if (onNavigate_) onNavigate_(rows[static_cast<size_t>(pick)].path);
        } else if (data >= 1000) {
            if (historyJump_) historyJump_(data - 1000, false);
        } else if (data >= 0 && historyJump_) {
            historyJump_(data, true);
        }
    }
    FreeRows(rows);
}

void NavBar::ShowViewMenu(POINT screen) {    (void)screen;
    std::vector<PopupRow> rows;
    for (int i = 0; i < static_cast<int>(kViewCount); ++i)
        rows.push_back({kViews[i].label, L"", nullptr, false, false, false, i});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    static const wchar_t* kSorts[] = {L"按名称排序", L"按日期排序", L"按类型排序", L"按大小排序"};
    static const wchar_t* kGroups[] = {L"不分组", L"按类型分组", L"按日期分组", L"按大小分组"};
    for (int i = 0; i < 4; ++i)
        rows.push_back({kSorts[i], L"", nullptr, false, false, false, 100 + i});
    rows.push_back({L"", L"", nullptr, false, true, false, -1});
    for (int i = 0; i < 4; ++i)
        rows.push_back({kGroups[i], L"", nullptr, false, false, false, 200 + i});

    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    int pick = ShowPopupList(hwnd_, *theme_, at, rows, 0);
    if (pick >= 0 && pick < static_cast<int>(rows.size())) {
        int data = rows[static_cast<size_t>(pick)].data;
        if (data >= 200) {
            if (onSortGroup_) onSortGroup_(1, data - 200);
        } else if (data >= 100) {
            if (onSortGroup_) onSortGroup_(0, data - 100);
        } else if (data >= 0 && onView_) {
            // 以前这里有两个分支，条件互补但函数体逐字相同 —— 保留一个。
            onView_(kViews[data].fvm, kViews[data].icon);
        }
    }
    FreeRows(rows);
}

void NavBar::ShowViewMenuNow() {
    RECT anchor = fieldRc_;
    for (const auto& b : buttons_)
        if (b.id == kView) anchor = b.rc;
    POINT at{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &at);
    ShowViewMenu(at);
}
