#include "popup_list.h"

#include "platform_win.h"
#include "popup_toggle.h"

#include "ui_draw.h"

namespace {

using id::kPopupClass;

struct PopupState {
    const std::vector<PopupRow>* rows = nullptr;   // 平铺模式数据
    const Theme* theme = nullptr;
    HWND self = nullptr;
    HWND list = nullptr;
    int hot = -1;
    bool done = false;
    bool wasActive = false;
    bool downInList = false;  // 必须先在本列表按下，抬起才算选中（否则开菜单那一下的抬起会立刻误触发）
    int picked = -1;
    int itemH = 24;
    int sepPx = 2;   // 分隔线只占 2px：它只是分组提示，不该和普通条目一样高
    int iconS = 16;
    bool above = false;  // true = 向上展开（底边锚定），树展开时不能盖住锚点控件

    // 树模式（面包屑文件夹下拉）：对应 Exbar.cpp 的 PopupListState treeMode 分支
    bool treeMode = false;
    std::vector<TreeNode> tree;     // 所有节点（含懒加载出的子节点）
    std::vector<int> visible;       // 当前可见节点索引（显示顺序）
    TreeChildrenFn treeChildren;
    std::wstring treePicked;        // 树模式选中的 path（空=取消）
    int hotExpander = -1;           // 悬停在展开器上的 visible 行号
    int curSel = -1;                // 键盘选择（visible 行号）

    // 「再点即收」的锚点键（见 popup_toggle.h）。空 = 不参与判定。
    std::wstring toggleKey;
    // 用户主动结束（选中 / Esc / Enter）→ 关掉时要清掉连点记录。外部点击关闭不清，
    // 那条记录正是给紧随其后的重入 Show* 判"这一下是收起"用的。
    bool deliberateClose = false;
};

// ---------- 树模式辅助（对齐 Exbar.cpp 18371-19312） ----------

// 右侧展开器矩形：行右端留出 chevron 命中区（mod PopupExpanderRect）
static RECT ExpanderRect(const RECT& irc, const Theme& th) {
    int pad = th.S(4);
    int bw = th.S(18);
    RECT e;
    e.left = irc.right - pad - bw;
    e.top = irc.top;
    e.right = irc.right - pad;
    e.bottom = irc.bottom;
    return e;
}

// 深度优先收集可见节点：折叠节点的子树不进入 visible（mod PopupCollectVisible）
static void CollectVisible(const std::vector<TreeNode>& tree, int ni, std::vector<int>& out) {
    out.push_back(ni);
    if (tree[static_cast<size_t>(ni)].expanded) {
        for (int c : tree[static_cast<size_t>(ni)].children)
            CollectVisible(tree, c, out);
    }
}

static void CollectAllVisible(PopupState* st) {
    st->visible.clear();
    for (size_t i = 0; i < st->tree.size(); ++i)
        if (st->tree[i].parent < 0)
            CollectVisible(st->tree, static_cast<int>(i), st->visible);
}

// 懒加载子节点（同步；ExBarBrowser 的 NavModel::ChildFolders 是同步枚举）
static void EnsureLoaded(PopupState* st, int ni) {
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

static int ItemFromPoint(HWND list, int x, int y) {
    LRESULT r = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    if (HIWORD(r) != 0) return -1;
    return static_cast<int>(LOWORD(r));
}

static int RowCount(const PopupState* st) {
    return st->treeMode ? static_cast<int>(st->visible.size())
                        : static_cast<int>(st->rows->size());
}

// 分隔线/禁用行是 inert，不高亮也不可选（mod PopupRowInert）
static bool RowInert(const PopupState* st, int idx) {
    if (st->treeMode) return false;
    if (idx < 0 || idx >= static_cast<int>(st->rows->size())) return true;
    const PopupRow& r = (*st->rows)[static_cast<size_t>(idx)];
    return r.separator || r.disabled;
}

// 单行像素高：分隔线只占 sepPx，普通条目占 itemH。
// 树模式没有分隔行，恒为 itemH。规则本体在 popup_list.h（纯函数，可单测）。
static int RowHeight(const PopupState* st, int idx) {
    if (st->treeMode || idx < 0 || idx >= static_cast<int>(st->rows->size())) return st->itemH;
    return PopupRowHeight((*st->rows)[static_cast<size_t>(idx)], st->itemH, st->sepPx);
}

// 前 n 行的累计高度（用于窗口尺寸 / 滚动定位）
static int TotalHeight(const PopupState* st, int n) {
    if (st->treeMode) {
        if (n > RowCount(st)) n = RowCount(st);
        return n * st->itemH;   // 树模式无分隔行
    }
    return PopupTotalHeight(*st->rows, n, st->itemH, st->sepPx);
}

// 按真实光标重算 hover / hotExpander（mod PopupRefreshHot）
static void RefreshHot(PopupState* st, HWND hList) {
    POINT cpt;
    GetCursorPos(&cpt);
    ScreenToClient(hList, &cpt);
    int idx = ItemFromPoint(hList, cpt.x, cpt.y);
    if (idx >= RowCount(st)) idx = -1;
    st->hot = idx;
    st->hotExpander = -1;
    if (st->treeMode && idx >= 0) {
        RECT irc;
        if (SendMessageW(hList, LB_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&irc)) >= 0) {
            RECT er = ExpanderRect(irc, *st->theme);
            if (PtInRect(&er, cpt)) st->hotExpander = idx;
        }
    }
}

// 窗口高度随可见行数自适应（mod PopupResizeToFit），最多 20 行
static void ResizeToFit(PopupState* st) {
    const int maxVis = 20;
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
            SendMessageW(st->list, LB_SETITEMDATA, id,
                         static_cast<LPARAM>(st->visible[static_cast<size_t>(vi)]));
            SendMessageW(st->list, LB_SETITEMHEIGHT, id, st->itemH);
        }
    }
    SendMessageW(st->list, WM_SETREDRAW, TRUE, 0);
    SendMessageW(st->list, LB_SETTOPINDEX, topIdx, 0);
    ResizeToFit(st);
    RefreshHot(st, st->list);
    InvalidateRect(st->list, nullptr, FALSE);
}

// 提交选中行（mod PopupPickRow）
static void PickRow(PopupState* st, HWND hList, int vi) {
    if (vi < 0 || vi >= RowCount(st)) return;
    if (st->treeMode) {
        int ni = st->visible[static_cast<size_t>(vi)];
        st->treePicked = st->tree[static_cast<size_t>(ni)].path;
    } else {
        if (RowInert(st, vi)) return;
        st->picked = vi;
    }
    st->deliberateClose = true;   // 选中 = 主动结束，关掉时清连点记录
    st->done = true;
    PostMessageW(st->self, WM_CLOSE, 0, 0);
}

static void ToggleExpand(PopupState* st, int vi) {
    if (!st->treeMode || vi < 0 || vi >= static_cast<int>(st->visible.size())) return;
    int ni = st->visible[static_cast<size_t>(vi)];
    TreeNode& n = st->tree[static_cast<size_t>(ni)];
    if (!n.hasChildren) return;            // 叶子：展开器区落到行主体导航
    if (!n.loaded) EnsureLoaded(st, ni);
    st->tree[static_cast<size_t>(ni)].expanded = !st->tree[static_cast<size_t>(ni)].expanded;
    st->curSel = -1;                        // 鼠标接管键盘焦点
    RebuildTree(st);
}

LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* st = reinterpret_cast<PopupState*>(ref);
    switch (m) {
    case WM_MOUSEMOVE: {
        int idx = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx >= RowCount(st)) idx = -1;
        if (RowInert(st, idx)) idx = -1;
        int newHotExp = -1;
        if (st->treeMode && idx >= 0) {
            RECT irc;
            if (SendMessageW(h, LB_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&irc)) >= 0) {
                RECT er = ExpanderRect(irc, *st->theme);
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (PtInRect(&er, pt)) newHotExp = idx;
            }
        }
        bool changed = false;
        if (idx >= 0 && st->curSel != -1 && st->curSel != idx) { st->curSel = -1; changed = true; }
        if (newHotExp != st->hotExpander) { st->hotExpander = newHotExp; changed = true; }
        if (idx != st->hot) { st->hot = idx; changed = true; }
        if (changed) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (st->hot != -1 || st->hotExpander != -1) {
            st->hot = -1;
            st->hotExpander = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // 滚动让行在静止光标下移动，hover 必须按真实光标重算
        LRESULT r = DefSubclassProc(h, m, w, l);
        RefreshHot(st, h);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_LBUTTONDOWN:
        st->downInList = true;
        break;
    case WM_LBUTTONUP: {
        if (!st->downInList) return 0;
        st->downInList = false;
        int idx = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx < 0 || idx >= RowCount(st) || RowInert(st, idx)) return 0;

        if (st->treeMode) {
            int vi = idx;
            int ni = st->visible[static_cast<size_t>(vi)];
            RECT irc;
            if (SendMessageW(h, LB_GETITEMRECT, vi, reinterpret_cast<LPARAM>(&irc)) >= 0) {
                RECT er = ExpanderRect(irc, *st->theme);
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (PtInRect(&er, pt) && st->tree[static_cast<size_t>(ni)].hasChildren) {
                    ToggleExpand(st, vi);   // 就地展开/收回，不关闭弹窗
                    return 0;
                }
            }
            PickRow(st, h, vi);                // 行主体：导航
            return 0;
        }
        PickRow(st, h, idx);
        return 0;
    }
    case WM_KEYDOWN: {
        int count = RowCount(st);
        if (w == VK_ESCAPE) {
            st->deliberateClose = true;
            st->done = true;
            PostMessageW(st->self, WM_CLOSE, 0, 0);
            return 0;
        }
        if (w == VK_DOWN || w == VK_UP) {
            int step = (w == VK_DOWN) ? 1 : -1;
            // 无光标时：向下从首行前起算，向上从末行后起算
            int cur = (st->curSel < 0) ? (step > 0 ? -1 : count) : st->curSel;
            cur += step;
            if (cur < 0) cur = 0;
            if (cur >= count) cur = count - 1;
            // 跳过分隔线/禁用行：它们被 RowInert 挡着选不中，光标停上去只会
            // 表现为"按回车没反应"。方向键要落到真正的可选行上。
            while (cur >= 0 && cur < count && RowInert(st, cur)) cur += step;
            if (cur < 0 || cur >= count) {
                // 该方向到头了（末尾可能是分隔行）：退回原选中行，保持不动
                cur = st->curSel;
                while (cur >= 0 && cur < count && RowInert(st, cur)) cur -= step;
            }
            if (cur < 0 || cur >= count || RowInert(st, cur)) {
                st->curSel = -1;
                SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            } else {
                st->curSel = cur;
                SendMessageW(h, LB_SETCURSEL, cur, 0);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_RETURN || w == VK_SPACE) {
            if (st->curSel >= 0) PickRow(st, h, st->curSel);
            else {
                st->deliberateClose = true;
                st->done = true;
                PostMessageW(st->self, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        if (st->treeMode && (w == VK_RIGHT || w == VK_LEFT)) {
            if (st->curSel >= 0 && st->curSel < count) {
                int vi = st->curSel;
                int ni = st->visible[static_cast<size_t>(vi)];
                const TreeNode& n = st->tree[static_cast<size_t>(ni)];
                if (w == VK_RIGHT && n.hasChildren && !n.expanded) {
                    ToggleExpand(st, vi);
                    // mod 18647-18649：RESETCONTENT 清掉了光标，键盘展开后要恢复选中行
                    st->curSel = vi;
                    SendMessageW(h, LB_SETCURSEL, vi, 0);
                    InvalidateRect(h, nullptr, FALSE);
                } else if (w == VK_LEFT) {
                    if (n.expanded) {
                        ToggleExpand(st, vi);
                        st->curSel = vi;
                        SendMessageW(h, LB_SETCURSEL, vi, 0);
                        InvalidateRect(h, nullptr, FALSE);
                    } else if (n.parent >= 0) {
                        // 收起态按左键 = 选中父节点（Explorer 树行为）
                        int pi = n.parent;
                        for (int v = 0; v < static_cast<int>(st->visible.size()); ++v) {
                            if (st->visible[static_cast<size_t>(v)] == pi) {
                                st->curSel = v;
                                SendMessageW(h, LB_SETCURSEL, v, 0);
                                InvalidateRect(h, nullptr, FALSE);
                                break;
                            }
                        }
                    }
                }
            }
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<PopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MEASUREITEM: {
        // LBS_OWNERDRAWVARIABLE 下每加一行都会先问高度。按行号区分：
        // 分隔线 sepPx，其余 itemH。不处理这一步的话插入瞬间行高未定，
        // 列表会先按默认高度排一遍再接我们的 LB_SETITEMHEIGHT，视觉上抖一下。
        if (!st) break;
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        mis->itemHeight = static_cast<UINT>(RowHeight(st, static_cast<int>(mis->itemID)));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (st) FillRect(dc, &rc, st->theme->brList);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCPAINT: {
        if (!st) break;
        HDC dc = GetWindowDC(h);
        RECT rc;
        GetWindowRect(h, &rc);
        // 四条边而不是 Rectangle+NULL_BRUSH：复用唯一的分隔线原语，少一套 pen/brush 样板。
        const int w = rc.right - rc.left;
        const int hh = rc.bottom - rc.top;
        RECT t{0, 0, w, 0};
        RECT b{0, hh - 1, w, hh - 1};
        RECT lf{0, 0, 0, hh};
        RECT rt{w - 1, 0, w - 1, hh};
        ui::DrawSeparator(dc, t, false, st->theme->line);
        ui::DrawSeparator(dc, b, false, st->theme->line);
        ui::DrawSeparator(dc, lf, true, st->theme->line);
        ui::DrawSeparator(dc, rt, true, st->theme->line);
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_DRAWITEM: {
        if (!st) return TRUE;
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);

        if (st->treeMode) {
            if (dis->itemID >= st->visible.size()) return TRUE;
            int ni = st->visible[static_cast<size_t>(dis->itemID)];
            const TreeNode& n = st->tree[static_cast<size_t>(ni)];
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            const bool hot = static_cast<int>(dis->itemID) == st->hot;
            const bool sel = static_cast<int>(dis->itemID) == st->curSel;
            // 画刷来自 Theme（唯一持有者），不再每行 CreateSolidBrush/DeleteObject。
            FillRect(dc, &rc, sel ? st->theme->brListSel
                                  : (hot ? st->theme->brListHot : st->theme->brList));

            SetBkMode(dc, TRANSPARENT);
            const int indent = rc.left + st->theme->S(6) + n.depth * st->theme->S(16);
            if (n.icon) {
                RECT ir{indent, rc.top, indent + st->iconS, rc.bottom};
                ui::DrawIconCentered(dc, n.icon, ir, st->iconS);
            }
            const RECT tr{indent + st->iconS + st->theme->S(4), rc.top,
                          rc.right - st->theme->S(26), rc.bottom};
            ui::DrawTextRow(dc, st->theme->font, n.label, tr, st->theme->text,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            if (n.hasChildren) {
                RECT er = ExpanderRect(rc, *st->theme);
                int cx = (er.left + er.right) / 2;
                int cy = (er.top + er.bottom) / 2;
                bool expHot = static_cast<int>(dis->itemID) == st->hotExpander;
                COLORREF clr = expHot ? st->theme->text : st->theme->chevron;
                // 与地址栏分隔符完全同一个原语：展开 down(dir=1)，收起 left(dir=0)
                ui::DrawChevron(dc, cx, cy, st->theme->iconChevron(), clr, n.expanded ? 1 : 0);
            }
            return TRUE;
        }

        if (dis->itemID == static_cast<UINT>(-1) || dis->itemID >= st->rows->size()) return TRUE;
        const PopupRow& row = (*st->rows)[dis->itemID];
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;

        if (row.separator) {
            // 行高只有 sepPx（默认 2px）：整行铺底色，横线画在行的垂直中心。
            // 上下各留 1px 底色 = 视觉上就是"条目之间 2px 的间隙 + 一条分隔线"，
            // 不再是原来那样占满一整行 24px 的空白。
            FillRect(dc, &rc, st->theme->brList);
            const int y = (rc.top + rc.bottom) / 2;
            RECT hr{rc.left + st->theme->S(8), y, rc.right - st->theme->S(8), y};
            ui::DrawSeparator(dc, hr, false, st->theme->line);
            return TRUE;
        }

        const bool sel = (dis->itemState & ODS_SELECTED) != 0;
        const bool hot = static_cast<int>(dis->itemID) == st->hot;
        FillRect(dc, &rc, sel ? st->theme->brListSel
                              : (hot ? st->theme->brListHot : st->theme->brList));

        SetBkMode(dc, TRANSPARENT);
        int x = rc.left + st->theme->S(8);
        if (row.icon) {
            RECT ir{x, rc.top, x + st->iconS, rc.bottom};
            ui::DrawIconCentered(dc, row.icon, ir, st->iconS);
        }
        x += st->iconS + st->theme->S(8);

        const RECT tr{x, rc.top, rc.right - st->theme->S(row.folder ? 26 : 10), rc.bottom};
        ui::DrawTextRow(dc, st->theme->font, row.label, tr,
                        row.disabled ? st->theme->disabled : st->theme->text,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (row.folder) {
            int cx = rc.right - st->theme->S(16);
            int cy = (rc.top + rc.bottom) / 2;
            // 与地址栏分隔符共用通用原语 DrawChevron；rest 同形态，选中提亮
            ui::DrawChevron(dc, cx, cy, st->theme->iconChevron(),
                            hot || sel ? st->theme->text : st->theme->chevron, 0);
        }
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX: {
        if (!st) break;
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, st->theme->text);
        SetBkColor(dc, st->theme->listBg);
        return reinterpret_cast<LRESULT>(st->theme->brList);
    }
    case WM_ACTIVATE:
        if (!st) return 0;
        if (LOWORD(w) == WA_ACTIVE || LOWORD(w) == WA_CLICKACTIVE) {
            st->wasActive = true;
        } else if (LOWORD(w) == WA_INACTIVE && st->wasActive) {
            st->done = true;
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (st) st->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureClass() { win::RegisterClassOnce(kPopupClass, PopupProc, CS_DROPSHADOW); }

// 计算弹层宽度：按最宽标签 + 图标槽 + 边距
static int MeasureWidth(HWND owner, const Theme& theme,
                        const std::vector<PopupRow>* rows,
                        const std::vector<TreeNode>* tree,
                        int iconS, bool exactWidth, int minWidthPx) {
    // 量字走唯一原语（ui::MeasureText 内部选字体并还原）
    HDC dc = GetDC(owner);
    int textW = 0;
    if (tree) {
        for (const auto& n : *tree) {
            textW = std::max<int>(textW,
                                  ui::MeasureText(dc, theme.font, n.label) +
                                      n.depth * theme.S(16));
        }
    } else if (rows) {
        for (const auto& r : *rows) {
            if (r.separator) continue;
            textW = std::max<int>(textW, ui::MeasureText(dc, theme.font, r.label));
        }
    }
    ReleaseDC(owner, dc);

    int width = textW + iconS + 8 + 8 + 26;
    int minW = minWidthPx > 0 ? minWidthPx : theme.S(160);
    if (exactWidth) {
        width = std::max(minW, theme.S(120));
    } else {
        width = std::max(width, minW);
        width = std::min(width, theme.S(460));
    }
    return width;
}

// 公共的窗口创建 + 模态循环
static void RunModal(PopupState& st, HWND owner, POINT screen, int width, int height) {
    HMONITOR mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = screen.x;
    int y = screen.y;
    if (x + width > mi.rcWork.right) x = static_cast<int>(mi.rcWork.right) - width;
    if (x < mi.rcWork.left) x = static_cast<int>(mi.rcWork.left);
    if (y + height > mi.rcWork.bottom) {
        y = screen.y - height;
        st.above = true;
    }
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    // WS_EX_COMPOSITED：整窗自绘 + 子 LISTBOX 时，先画到离屏再合成，避免
    // "先白后深"的闪一帧（深色主题下特别刺眼）。
    st.self = win::CreatePopup(kPopupClass, owner, &st,
                               WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_COMPOSITED, WS_POPUP, x, y,
                               width, height);
    if (!st.self) return;
    // 内缩 1px：WM_NCPAINT 画的 1px 边框在客户区外沿，LISTBOX 满铺会把它盖掉
    // （视觉上"菜单没有边框"，与 mod 的圆角描边不一致）。
    st.list = CreateWindowExW(0, L"LISTBOX", L"",
                              WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWVARIABLE | LBS_NOTIFY |
                                  LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                              1, 1, width - 2, height - 2, st.self, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!st.list) {
        DestroyWindow(st.self);
        st.self = nullptr;
        return;
    }

    // 原生 LISTBOX 的滚动条由系统绘制：不设主题时走浅色经典样式，
    // 在深色弹层上是一条亮灰宽条（与 mod 对齐：DarkMode_Explorer 下
    // 滚动条变深色且为 hover 展开的窄条，颜色/宽度一次解决）。
    AllowDarkForWindow(st.list, st.theme->dark);
    win::ThemeControl(st.list, st.theme->dark);

    SendMessageW(st.list, WM_SETFONT, reinterpret_cast<WPARAM>(st.theme->font), TRUE);
    // 可变行高：分隔线按 sepPx 插入，条目按 itemH（LBS_OWNERDRAWVARIABLE 下
    // 每行都要显式设置，否则系统按默认 0 高插入 → 整列表塌成一条）。
    SendMessageW(st.list, LB_SETITEMHEIGHT, 0, st.itemH);
    if (st.treeMode) {
        CollectAllVisible(&st);
        for (int ni : st.visible) {
            int id = static_cast<int>(SendMessageW(st.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"")));
            if (id >= 0) {
                SendMessageW(st.list, LB_SETITEMDATA, id, static_cast<LPARAM>(ni));
                SendMessageW(st.list, LB_SETITEMHEIGHT, id, st.itemH);
            }
        }
    } else {
        for (size_t i = 0; i < st.rows->size(); ++i) {
            LRESULT idx = SendMessageW(st.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>((*st.rows)[i].label.c_str()));
            if (idx >= 0) {
                SendMessageW(st.list, LB_SETITEMDATA, idx, static_cast<LPARAM>(i));
                SendMessageW(st.list, LB_SETITEMHEIGHT, idx,
                             RowHeight(&st, static_cast<int>(i)));
            }
        }
    }
    SetWindowSubclass(st.list, ListSubclass, 1, reinterpret_cast<DWORD_PTR>(&st));
    if (st.treeMode) ResizeToFit(&st);

    ShowWindow(st.self, SW_SHOW);
    SetForegroundWindow(st.self);
    SetFocus(st.list);

    // 窗口真的建起来了才记"这一锚点刚开过" —— 记在建窗前会在创建失败时留下
    // 脏记录，让用户下一次点击白白被吃掉。
    popup::NoteOpened(st.toggleKey.c_str());

    // 对齐 mod 19298-19301：GetMessage 返回 -1 是错误（不能继续循环），
    // 返回 0 是 WM_QUIT（必须原样 PostQuitMessage 交还宿主）。
    MSG msg;
    int gm;
    while (!st.done && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) PostQuitMessage(static_cast<int>(msg.wParam));

    RemoveWindowSubclass(st.list, ListSubclass, 1);
    if (IsWindow(st.self)) DestroyWindow(st.self);

    // 主动结束（选中 / Esc / Enter）要清掉连点记录：否则"Esc 关掉、马上又想看"
    // 会被下一次 WasJustOpened 误判成连点，第一下没反应。
    // 外部点击关闭不清 —— 那条记录正是留给紧随其后的重入 Show* 判"这一下是收起"的。
    if (st.deliberateClose) popup::NoteClosedDeliberate(st.toggleKey.c_str());
}

void FreeTree(std::vector<TreeNode>& tree) {
    for (auto& n : tree)
        if (n.icon) DestroyIcon(n.icon);
    tree.clear();
}

}  // namespace

int ShowPopupList(HWND owner, const Theme& theme, POINT screen,
                  const std::vector<PopupRow>& rows, int minWidthPx, bool exactWidth,
                  int maxRows, const wchar_t* toggleKey) {
    if (rows.empty()) return -1;

    // 「再点即收」：连点第二下直接当"取消"返回，不建窗口、不改任何状态。
    // 判定必须在这里（建窗口之前），见 popup_toggle.h 关于消息顺序的说明。
    if (popup::WasJustOpened(toggleKey)) return -1;

    EnsureClass();

    PopupState st;
    st.toggleKey = toggleKey ? toggleKey : L"";
    st.rows = &rows;
    st.theme = &theme;
    st.itemH = theme.S(24);
    st.sepPx = std::max(1, theme.S(2));   // 分隔线行高：2px（DPI 缩放下不为 0）
    st.iconS = theme.iconSmall();

    int width = MeasureWidth(owner, theme, &rows, nullptr, st.iconS, exactWidth, minWidthPx);
    // 上限至少 1 行：调用方传 0/负数时不能算出 0 高窗口（那样一个弹层都看不见）。
    if (maxRows < 1) maxRows = 1;
    int visible = std::min<int>(static_cast<int>(rows.size()), maxRows);
    int height = TotalHeight(&st, visible) + 2;

    RunModal(st, owner, screen, width, height);
    return st.picked;
}

std::wstring ShowFolderTree(HWND owner, const Theme& theme, POINT screen,
                            std::vector<TreeNode> topNodes, TreeChildrenFn children,
                            int minWidthPx, const wchar_t* toggleKey) {
    if (topNodes.empty()) return std::wstring();
    if (popup::WasJustOpened(toggleKey)) {
        // 收起路径也要释放调用方移交的 icon，否则每次"再点收起"漏一批 HICON。
        FreeTree(topNodes);
        return std::wstring();
    }

    EnsureClass();

    PopupState st;
    st.toggleKey = toggleKey ? toggleKey : L"";
    st.theme = &theme;
    st.treeMode = true;
    st.tree = std::move(topNodes);
    st.treeChildren = std::move(children);
    st.itemH = theme.S(24);
    st.iconS = theme.iconSmall();

    int width = MeasureWidth(owner, theme, nullptr, &st.tree, st.iconS, false, minWidthPx);
    int topCount = 0;
    for (const auto& n : st.tree) if (n.parent < 0) ++topCount;
    int visible = std::min<int>(topCount, 20);
    int height = visible * st.itemH + 2;  // 树模式无分隔行，恒为 itemH

    RunModal(st, owner, screen, width, height);
    std::wstring result = st.treePicked;
    FreeTree(st.tree);   // 弹层统一销毁整棵树（含懒加载子节点）的 icon
    return result;
}
