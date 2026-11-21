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
    bool downOutside = false; // 按下点落在列表外（= 要收起）；等 UP 再关，见 WM_LBUTTONDOWN
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

    // 初态：预选第几行 / 是否滚到它（见 popup_list.h 的 PopupInitial）。
    int selectRow = -1;
    bool scrollTo = true;
    int maxRows = 20;   // 可见行上限，预选居中滚动时要用

    // 键入首字母跳转（type-ahead）的累积缓冲。
    //
    // 为什么必须自己做：本列表是 LBS_OWNERDRAWVARIABLE 且**插入的是空字符串**
    // （文字在 WM_DRAWITEM 里按行号自绘），系统那套"键入字符 → LB_FINDSTRING"
    // 的默认行为完全失效 —— 它没有任何字符串可比。菜单 20+ 行时用户只能靠方向键挪，
    // 而 TC / Opus / 资源管理器的菜单都支持敲字母定位。
    std::wstring typeBuf;
    ULONGLONG typeTick = 0;
    // 连续按键超过这个间隔就当作"重新开始一个字"，否则累积成前缀。
    static constexpr ULONGLONG kTypeResetMs = 900;
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
        if (SendMessageW(hList, LB_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&irc)) >= 0) {
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

    // 键入首字母跳转：从当前行往下找第一个 label 以 buf 开头的可选行，找不到就绕回顶部。
    // 口令与规则见 PopupState::typeBuf 的注释（本列表插的是空字符串，
    // 系统默认的 LB_FINDSTRING 在这里完全失效）。
    auto typeAhead = [st, h](const std::wstring& buf) {
        if (buf.empty()) return false;
        const int n = RowCount(st);
        if (n <= 0) return false;
        // 从当前行的下一行开始找（反复按同一个字母 = 在该字母的条目间轮换），
        // 与资源管理器/TC 的行为一致；没有当前行时从 0 开始。
        const int start = (st->curSel >= 0) ? st->curSel : -1;
        for (int pass = 0; pass < 2; ++pass) {
            const int from = (pass == 0) ? start + 1 : 0;
            const int to = (pass == 0) ? n : start + 1;
            for (int i = from; i < to; ++i) {
                if (RowInert(st, i)) continue;
                const std::wstring& label = st->treeMode
                    ? st->tree[static_cast<size_t>(st->visible[static_cast<size_t>(i)])].label
                    : (*st->rows)[static_cast<size_t>(i)].label;
                if (str::IStartsWith(label, buf)) {
                    st->curSel = i;
                    SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                    InvalidateRect(h, nullptr, FALSE);
                    return true;
                }
            }
        }
        return false;
    };

    switch (m) {
    case WM_CHAR: {
        if (!st) break;
        // 只吃可打印字符。控制字符（Ctrl 组合键产生的 1..26）交给下面，
        // 它们属于快捷键而不是"定位文本"。
        if (w < L' ' || w == 0x7F) break;
        const ULONGLONG now = GetTickCount64();
        // 【间隔判定用 GetTickCount64 而不是"字符相同就重置"】用户敲 "ss" 想在
        // 两个 s 开头的条目间轮换，与"打一个两字前缀 ss..."是同一串按键 ——
        // 只有时间能区分意图。900ms 与系统 type-ahead 的量级相当。
        if (now - st->typeTick > PopupState::kTypeResetMs) st->typeBuf.clear();
        st->typeTick = now;
        st->typeBuf.push_back(static_cast<wchar_t>(w));
        if (!typeAhead(st->typeBuf)) {
            // 这个前缀没人匹配：退回单字符再试一次（否则打错一个字母后
            // 后续所有输入都在拼一个永远匹配不上的串，越打越没反应）。
            if (st->typeBuf.size() > 1) {
                st->typeBuf.assign(1, static_cast<wchar_t>(w));
                typeAhead(st->typeBuf);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int idx = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx >= RowCount(st)) idx = -1;
        if (RowInert(st, idx)) idx = -1;
        int newHotExp = -1;
        if (st->treeMode && idx >= 0) {
            RECT irc;
            if (SendMessageW(h, LB_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&irc)) >= 0) {
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
    case WM_CAPTURECHANGED:
        // 捕获转移。**必须区分两种情形**，否则会把正常点击变成"菜单自己关掉"：
        //
        //   lParam == 本列表 —— 原生 LISTBOX 按下时会自己 SetCapture（为了支持
        //     拖选）。那是我们自己这一侧的内部行为，捕获仍在列表手里，忽略即可。
        //   其他 —— 被系统弹窗 / 另一个程序抢走。此时弹层既收不到后续鼠标、
        //     也不会再失活关闭，会变成"关不掉的菜单"，必须跟着关。
        //
        // 主动关闭的路径（点在列表外、Esc、选中行）都是**先**置 done 再
        // ReleaseCapture，所以这里看到 done=true 不会重复 Post。
        if (reinterpret_cast<HWND>(l) == h) return 0;
        if (!st->done) {
            st->done = true;
            PostMessageW(st->self, WM_CLOSE, 0, 0);
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
    case WM_RBUTTONDOWN: {
        // 【再点即收的核心】列表持有鼠标捕获（见 RunModal 的 SetCapture），
        // 所以弹层开着期间**所有**鼠标按下都先送到这里 —— 不管光标在列表内
        // 还是跑到下面的锚点控件上（这正是捕获的语义，也是拖拽能跑出窗口的原因）。
        //
        //   点在列表内 → 正常按下，等 UP 选行（右键不选行，见下面的 UP）。
        //   点在列表外 → 这就是"要收起"的那一下。
        //
        // 【为什么 DOWN 里不直接关】必须等到 UP 再关并释放捕获，否则 UP 会落到
        // 被点的控件上，而本工程各栏（书签/标签/状态栏/面包屑）都在 UP 里执行动作
        // —— 于是"点一下收起菜单"会顺带打开那个书签、切走那个标签。
        // 这就是资源管理器"菜单开着时点别处只关菜单、不触发别的"的语义。
        //
        // 右键一并接管：右键菜单（标签/书签/状态栏）开着时，用户的下一手可能是
        // 右键点别处。不接管的话这条消息会漏给 ListBox 的默认处理，行为不可控
        // （轻则无反应，重则弹层与下层控件同时响应）。
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT rc;
        GetClientRect(h, &rc);
        if (!PtInRect(&rc, pt)) {
            // 只记下"这一下是外点"，捕获继续保持到 UP。
            st->downOutside = true;
            return 0;
        }
        st->downOutside = false;
        st->downInList = true;
        break;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT rc;
        GetClientRect(h, &rc);
        const bool upInside = PtInRect(&rc, pt) != FALSE;

        // 【"菜单一闪而过"的真因就在这里】
        //
        // 导航栏各下拉是**按下**就弹出的（与资源管理器一致）。弹层一建好就
        // SetCapture(list)，于是**开菜单那次点击的松开**也送到列表，而光标还
        // 停在锚点上 —— 锚点在列表之外。若把这一下当成"点了外面"，菜单会在
        // 松手瞬间自己关掉，用户看到的就是"一闪而过"。
        //
        // 这一次 UP 不是新点击，它是开菜单那次点击的后半截。
        // TrackPopupMenu 也是先等这一下松开，之后才把点击当作"关闭"。
        if (PopupUpIsOpeningRelease(st->downInList, st->downOutside, upInside)) {
            return 0;
        }

        const bool outside = st->downOutside || !upInside;
        if (outside) {
            // 先置 done 再 ReleaseCapture：ReleaseCapture 会**同步**派发
            // WM_CAPTURECHANGED，那里的处理看到 done=true 就不会再 Post 一次。
            st->downOutside = false;
            st->downInList = false;
            st->done = true;
            if (GetCapture() == h) ReleaseCapture();
            PostMessageW(st->self, WM_CLOSE, 0, 0);
            return 0;
        }
        if (!st->downInList) return 0;
        st->downInList = false;

        // 右键落在菜单内：只关菜单，不选行。
        // 与 TrackPopupMenu 同语义 —— 右键是"另起一次操作"，不该顺手执行当前项。
        if (m == WM_RBUTTONUP) {
            st->done = true;
            if (GetCapture() == h) ReleaseCapture();
            PostMessageW(st->self, WM_CLOSE, 0, 0);
            return 0;
        }

        int idx = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx < 0 || idx >= RowCount(st) || RowInert(st, idx)) return 0;

        if (st->treeMode) {
            const int vi = idx;
            // 名字带 hit 前缀：外层（本函数前半段）已有一个 ni，
            // 同名会让读者以为这是同一个量的延续。
            const int hitNi = st->visible[static_cast<size_t>(vi)];
            RECT irc;
            if (SendMessageW(h, LB_GETITEMRECT, static_cast<WPARAM>(vi),
                             reinterpret_cast<LPARAM>(&irc)) >= 0) {
                RECT er = ExpanderRect(irc, *st->theme);
                const POINT expPt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};   // 不叫 pt：外层已有
                if (PtInRect(&er, expPt) && st->tree[static_cast<size_t>(hitNi)].hasChildren) {
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
            st->done = true;
            PostMessageW(st->self, WM_CLOSE, 0, 0);
            return 0;
        }
        if (w == VK_DOWN || w == VK_UP || w == VK_HOME || w == VK_END || w == VK_PRIOR ||
            w == VK_NEXT) {
            const int step = (w == VK_DOWN) ? 1 : -1;
            int cur = st->curSel;
            if (w == VK_HOME) {
                cur = 0;
            } else if (w == VK_END) {
                cur = count - 1;
            } else {
                // 翻页：按"一次能看见几行"跳，而不是固定 5 行 ——
                // 菜单有 20+ 行时（命令面板/标签菜单）逐行挪一趟要按十几次。
                const int page = std::max(1, st->maxRows - 1);
                const int jump = (w == VK_PRIOR || w == VK_NEXT) ? page : 1;
                // 无光标时：向下从首行前起算，向上从末行后起算
                if (cur < 0) cur = step > 0 ? -jump : count + jump;
                else cur += step * jump;
            }
            if (cur < 0) cur = 0;
            if (cur >= count) cur = count - 1;
            // 跳过分隔线/禁用行：它们被 RowInert 挡着选不中，光标停上去只会
            // 表现为"按回车没反应"。按键要落到真正的可选行上。
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
                SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(cur), 0);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_RETURN || w == VK_SPACE) {
            if (st->curSel >= 0) PickRow(st, h, st->curSel);
            else {
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
                    SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(vi), 0);
                    InvalidateRect(h, nullptr, FALSE);
                } else if (w == VK_LEFT) {
                    if (n.expanded) {
                        ToggleExpand(st, vi);
                        st->curSel = vi;
                        SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(vi), 0);
                        InvalidateRect(h, nullptr, FALSE);
                    } else if (n.parent >= 0) {
                        // 收起态按左键 = 选中父节点（Explorer 树行为）
                        int pi = n.parent;
                        for (int v = 0; v < static_cast<int>(st->visible.size()); ++v) {
                            if (st->visible[static_cast<size_t>(v)] == pi) {
                                st->curSel = v;
                                SendMessageW(h, LB_SETCURSEL, static_cast<WPARAM>(v), 0);
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
        const int wndW = rc.right - rc.left;   // 不叫 w：本函数上半段已有一个 w
        const int hh = rc.bottom - rc.top;
        RECT t{0, 0, wndW, 0};
        RECT b{0, hh - 1, wndW, hh - 1};
        RECT lf{0, 0, 0, hh};
        RECT rt{wndW - 1, 0, wndW - 1, hh};
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
            } else if (n.glyph != ico::IconId::None) {
                RECT gr{indent, rc.top, indent + st->iconS, rc.bottom};
                ui::DrawGlyph(dc, n.glyph, gr, st->iconS, st->theme->text);
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
        } else if (row.glyph != ico::IconId::None) {
            // 与 HICON 共用同一条图标槽：不新建字体、不建临时图标，
            // 直接走 ui::DrawGlyph（内部选 GlyphFace 并还原字体）。
            RECT gr{x, rc.top, x + st->iconS, rc.bottom};
            ui::DrawGlyph(dc, row.glyph, gr, st->iconS,
                          row.disabled ? st->theme->disabled : st->theme->text);
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
        } else if (LOWORD(w) == WA_INACTIVE && st->wasActive && !st->done) {
            // 【不能一失活就关】判据见 popup_toggle.h。
            //
            // 点本程序自己的书签栏/标签栏时，系统**同步**发 WA_INACTIVE，
            // 而这次点击的 DOWN/UP 还没派发。若在这里就 PostMessage(WM_CLOSE)，
            // WM_CLOSE 会排在 DOWN 之前被处理 → 弹层先被销毁 → 捕获释放 →
            // UP 落到锚点控件 → 锚点在 UP 里又调一次 Show* → 菜单"关了又开"。
            // 这就是"每次点击都是重新弹出"的真因（不是时间窗、不是点击间隔）。
            //
            // 持有捕获 + 前景窗同线程（= 点的是本程序另一个窗口）时不关：
            // 那一组 DOWN/UP 由 ListSubclass 吞着，UP 判到"点在列表外"再关。
            if (popup::ShouldCloseOnDeactivate(st->self, st->list)) {
                st->done = true;
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
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

    // 高度必须先夹进工作区，再算 y —— 顺序反了会出下面这个洞：
    //   先按原高度决定"向上还是向下展开"，再把 y 推回 rcWork.top，
    //   于是 rcWork.top + height 越过工作区底边，最后几行和滚动条一起画到屏幕外，
    //   用户既点不到也滚不到（listbox 能滚，但滚动条本身在屏幕外）。
    // 标签右键菜单 22 行 × 24dp：125% DPI 下 660px，768 高的屏幕再去掉任务栏
    // 就顶到了边 —— 这不是理论风险。
    const int workH = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
    if (height > workH) height = workH;

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
                SendMessageW(st.list, LB_SETITEMDATA, static_cast<WPARAM>(id), static_cast<LPARAM>(ni));
                SendMessageW(st.list, LB_SETITEMHEIGHT, static_cast<WPARAM>(id), st.itemH);
            }
        }
    } else {
        for (size_t i = 0; i < st.rows->size(); ++i) {
            LRESULT idx = SendMessageW(st.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>((*st.rows)[i].label.c_str()));
            if (idx >= 0) {
                SendMessageW(st.list, LB_SETITEMDATA, static_cast<WPARAM>(idx), static_cast<LPARAM>(i));
                SendMessageW(st.list, LB_SETITEMHEIGHT, static_cast<WPARAM>(idx),
                             RowHeight(&st, static_cast<int>(i)));
            }
        }
    }
    SetWindowSubclass(st.list, ListSubclass, 1, reinterpret_cast<DWORD_PTR>(&st));
    if (st.treeMode) ResizeToFit(&st);

    // 初态：预选并滚到指定行。必须在**所有行都加完**之后做 ——
    // LB_SETCURSEL 会把列表滚到"能看见该行"的位置，提前设会被后续 ADDSTRING 覆盖。
    // LBS_OWNERDRAWVARIABLE + 跳过分隔行：分隔/禁用行不可选，落到它们身上时往下找。
    if (!st.treeMode && st.selectRow >= 0) {
        int row = st.selectRow;
        const int n = RowCount(&st);
        while (row < n && RowInert(&st, row)) ++row;
        if (row >= n) {
            row = st.selectRow;
            while (row >= 0 && RowInert(&st, row)) --row;
        }
        if (row >= 0 && row < n && !RowInert(&st, row)) {
            st.curSel = row;
            SendMessageW(st.list, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
            if (st.scrollTo) {
                // LB_SETCURSEL 只在"完全不可见"时才滚；部分可见时它不动。
                // 这里再显式保证整行可见（下拉里半截行读不出是哪一项）。
                SendMessageW(st.list, LB_SETTOPINDEX,
                             static_cast<WPARAM>(std::max(0, row - st.maxRows / 2)), 0);
            }
            LogDbg(L"[popup] 预选第 %d 行", row);
        }
    }

    ShowWindow(st.self, SW_SHOW);
    SetForegroundWindow(st.self);
    SetFocus(st.list);

    // 【再点即收的机制所在】把鼠标捕获交给列表。
    //
    // 弹层开着期间，**所有**鼠标按下都先送到列表（捕获的语义就是如此），
    // 不管光标落在列表内还是跑到下面的锚点控件上：
    //   · 列表内 → 正常选行（ListSubclass 的 WM_LBUTTONUP）。
    //   · 列表外 → ListSubclass 的 WM_LBUTTONDOWN 认出"在客户区之外"，
    //              吃掉这一下并关窗。锚点**收不到**这次点击 → 不会重入 Show*。
    //
    // 这就是 TrackPopupMenu / 组合框下拉 / 资源管理器菜单的行为：菜单开着时点别处，
    // 那一下只用来关菜单，不会同时触发被点的控件。与时间无关，因此不存在
    // "用户想收回时窗口早过期"的问题。
    SetCapture(st.list);
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

    if (GetCapture() == st.list) ReleaseCapture();
    RemoveWindowSubclass(st.list, ListSubclass, 1);
    // 注销要无条件做：不管这次是"选中/Esc"主动结束，还是"点外部/失活"被动结束，
    // 弹层没了的下一瞬间，同键的 Show* 都必须能正常打开。
    popup::NoteClosed(st.toggleKey.c_str());
    if (IsWindow(st.self)) DestroyWindow(st.self);
}

// 树节点的 icon 来自 ico::Cached（进程内共享），这里只清结构、不 DestroyIcon。
// 原来的 FreeTree 会销毁每个节点 HICON；改成缓存后若继续销毁，第二次展开同一
// 目录就会拿到悬空句柄 —— 生命周期收口在 icon_resolve.cpp 一处。
void ClearTree(std::vector<TreeNode>& tree) {
    tree.clear();
}

}  // namespace

int ShowPopupList(HWND owner, const Theme& theme, POINT screen,
                  const std::vector<PopupRow>& rows, int minWidthPx, bool exactWidth,
                  int maxRows, const wchar_t* toggleKey, PopupInitial init) {
    if (rows.empty()) return -1;

    // 「再点即收」的兜底：正常情况下捕获模型已经让第二次点击到不了这里
    // （它被列表吃掉了）。但主窗口若通过键盘/命令重入调 Show*，这里也要挡住，
    // 否则会叠出两个菜单。判据是"同键的弹层此刻还开着"，与时间无关。
    if (popup::IsOpen(toggleKey)) return -1;

    EnsureClass();

    PopupState st;
    st.toggleKey = toggleKey ? toggleKey : L"";
    st.rows = &rows;
    st.theme = &theme;
    st.itemH = theme.S(24);
    st.sepPx = std::max(1, theme.S(2));   // 分隔线行高：2px（DPI 缩放下不为 0）
    st.iconS = theme.iconSmall();
    st.selectRow = init.selectRow;
    st.scrollTo = init.scrollTo;

    int width = MeasureWidth(owner, theme, &rows, nullptr, st.iconS, exactWidth, minWidthPx);
    // 上限至少 1 行：调用方传 0/负数时不能算出 0 高窗口（那样一个弹层都看不见）。
    if (maxRows < 1) maxRows = 1;
    st.maxRows = maxRows;
    int visible = std::min<int>(static_cast<int>(rows.size()), maxRows);
    int height = TotalHeight(&st, visible) + 2;

    RunModal(st, owner, screen, width, height);
    return st.picked;
}

std::wstring ShowFolderTree(HWND owner, const Theme& theme, POINT screen,
                            std::vector<TreeNode> topNodes, TreeChildrenFn children,
                            int minWidthPx, const wchar_t* toggleKey) {
    if (topNodes.empty()) return std::wstring();
    // 与 ShowPopupList 同一理由：捕获模型已让第二次点击到不了这里，这条只是
    // 键盘/命令重入时的兜底。
    if (popup::IsOpen(toggleKey)) {
        // 收起路径也要清空节点（icon 归进程缓存，不在这里销毁）。
        ClearTree(topNodes);
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
    ClearTree(st.tree);   // 节点 icon 归 ico::Cached，弹层只清结构
    return result;
}
