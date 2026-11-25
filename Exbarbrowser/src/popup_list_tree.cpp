// 弹层树与几何：可见节点收集、懒加载、行高/总高、hover 重算、自适应尺寸、展开/收回。
// 与 popup_list_input.cpp 共用同一份 PopupState（见 popup_list_internal.h）；
// 本文件不处理鼠标捕获，也不决定"点这一行要导航到哪"。
#include "popup_list_internal.h"

#include "config.h"
#include "popup_list_draw.h"

// 深度优先收集可见节点：折叠节点的子树不进入 visible（mod PopupCollectVisible）
static void CollectVisible(const std::vector<TreeNode>& tree, int ni, std::vector<int>& out) {
    out.push_back(ni);
    if (tree[static_cast<size_t>(ni)].expanded) {
        for (int c : tree[static_cast<size_t>(ni)].children)
            CollectVisible(tree, c, out);
    }
}

void CollectAllVisible(PopupState* st) {
    st->visible.clear();
    for (size_t i = 0; i < st->tree.size(); ++i)
        if (st->tree[i].parent < 0)
            CollectVisible(st->tree, static_cast<int>(i), st->visible);
}

// 懒加载子节点（同步；ExBarBrowser 的 NavModel::ChildFolders 是同步枚举）
void EnsureLoaded(PopupState* st, int ni) {
    TreeNode& n = st->tree[static_cast<size_t>(ni)];
    if (n.loaded || !st->treeChildren) { n.loaded = true; return; }
    n.loaded = true;
    std::vector<TreeNode> kids = st->treeChildren(n.path);
    int baseDepth = st->tree[static_cast<size_t>(ni)].depth + 1;
    for (auto& k : kids) {
        k.depth = baseDepth;
        k.parent = ni;
        // push_back 可能重分配，始终按索引重新取父节点
        st->tree.push_back(std::move(k));
        int ci = static_cast<int>(st->tree.size()) - 1;
        st->tree[static_cast<size_t>(ni)].children.push_back(ci);
    }
    if (kids.empty()) st->tree[static_cast<size_t>(ni)].hasChildren = false;
}

int ItemFromPoint(HWND list, int x, int y) {
    LRESULT r = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    if (HIWORD(r) != 0) return -1;
    return static_cast<int>(LOWORD(r));
}

int RowCount(const PopupState* st) {
    return st->treeMode ? static_cast<int>(st->visible.size())
                        : static_cast<int>(st->rows->size());
}

// 分隔线/禁用行是 inert，不高亮也不可选（mod PopupRowInert）
bool RowInert(const PopupState* st, int idx) {
    if (st->treeMode) return false;
    if (idx < 0 || idx >= static_cast<int>(st->rows->size())) return true;
    const PopupRow& r = (*st->rows)[static_cast<size_t>(idx)];
    return r.separator || r.disabled;
}

// 单行像素高：分隔线只占 sepPx，普通条目占 itemH。
// 树模式没有分隔行，恒为 itemH。规则本体在 popup_list.h（纯函数，可单测）。
int RowHeight(const PopupState* st, int idx) {
    if (st->treeMode || idx < 0 || idx >= static_cast<int>(st->rows->size())) return st->itemH;
    return PopupRowHeight((*st->rows)[static_cast<size_t>(idx)], st->itemH, st->sepPx);
}

// 前 n 行的累计高度（用于窗口尺寸 / 滚动定位）
int TotalHeight(const PopupState* st, int n) {
    if (st->treeMode) {
        if (n > RowCount(st)) n = RowCount(st);
        return n * st->itemH;   // 树模式无分隔行
    }
    return PopupTotalHeight(*st->rows, n, st->itemH, st->sepPx);
}

// 按真实光标重算 hover / hotExpander（mod PopupRefreshHot）
void RefreshHot(PopupState* st, HWND hList) {
    POINT cpt;
    GetCursorPos(&cpt);
    ScreenToClient(hList, &cpt);
    int idx = ItemFromPoint(hList, cpt.x, cpt.y);
    if (idx >= RowCount(st)) idx = -1;
    st->hot = idx;
    st->hotExpander = -1;
    if (st->treeMode && idx >= 0) {
        RECT irc;
        if (SendMessageW(hList, LB_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&irc)) >= 0) {
            RECT er = PopupExpanderRect(irc, *st->theme);
            if (PtInRect(&er, cpt)) st->hotExpander = idx;
        }
    }
}

// 窗口高度随可见行数自适应（mod PopupResizeToFit），上限来自设置 popupMaxRows
void ResizeToFit(PopupState* st) {
    const int maxVis = Config::Get().s().popupMaxRows;
    int rows = std::min(RowCount(st), maxVis);
    int newH = TotalHeight(st, rows) + 2;
    RECT wr{};
    GetWindowRect(st->self, &wr);
    HMONITOR mon = MonitorFromWindow(st->self, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int w = wr.right - wr.left;
    int y = wr.top;
    if (st->above) {
        // 向上展开：底边固定，向上长/缩（否则展开树节点会一路盖住导航栏/书签栏）
        const int bottom = wr.top + (wr.bottom - wr.top);
        y = bottom - newH;
    } else if (y + newH > mi.rcWork.bottom) {
        y = mi.rcWork.bottom - newH;
    }
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(st->self, nullptr, wr.left, y, w, newH, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(st->list, nullptr, 1, 1, w - 2, newH - 2, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(st->self, nullptr, FALSE);
}

// 重建树的 visible 列表与 ListBox 内容（mod PopupRebuildTree）
static void RebuildTree(PopupState* st) {
    int topIdx = static_cast<int>(SendMessageW(st->list, LB_GETTOPINDEX, 0, 0));
    SendMessageW(st->list, WM_SETREDRAW, FALSE, 0);
    CollectAllVisible(st);
    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    for (int vi = 0; vi < static_cast<int>(st->visible.size()); ++vi) {
        int id = static_cast<int>(SendMessageW(st->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"")));
        if (id >= 0) {
            SendMessageW(st->list, LB_SETITEMDATA, static_cast<WPARAM>(id),
                         static_cast<LPARAM>(st->visible[static_cast<size_t>(vi)]));
            SendMessageW(st->list, LB_SETITEMHEIGHT, static_cast<WPARAM>(id), st->itemH);
        }
    }
    SendMessageW(st->list, WM_SETREDRAW, TRUE, 0);
    SendMessageW(st->list, LB_SETTOPINDEX, static_cast<WPARAM>(topIdx), 0);
    ResizeToFit(st);
    RefreshHot(st, st->list);
    InvalidateRect(st->list, nullptr, FALSE);
}

void ToggleExpand(PopupState* st, int vi) {
    if (!st->treeMode || vi < 0 || vi >= static_cast<int>(st->visible.size())) return;
    int ni = st->visible[static_cast<size_t>(vi)];
    TreeNode& n = st->tree[static_cast<size_t>(ni)];
    if (!n.hasChildren) return;            // 叶子：展开器区落到行主体导航
    if (!n.loaded) EnsureLoaded(st, ni);
    st->tree[static_cast<size_t>(ni)].expanded = !st->tree[static_cast<size_t>(ni)].expanded;
    st->curSel = -1;                        // 鼠标接管键盘焦点
    RebuildTree(st);
}
