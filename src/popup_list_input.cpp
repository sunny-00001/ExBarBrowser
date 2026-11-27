// 弹层输入状态机：type-ahead、鼠标捕获语义、键盘导航、选中提交。
// 与 popup_list_tree.cpp 共用同一份 PopupState（见 popup_list_internal.h）。
// 捕获顺序不可改：外部按下只记状态，等 UP 再关窗，避免同一下点击落到锚点控件。
#include "popup_list_internal.h"

#include "config.h"
#include "popup_list_draw.h"
#include "popup_toggle.h"

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

// 拖拽启动阈值不再在这里定义：已升为设置 popupDragThreshold（见 config.h），
// 原因是"点一下"与"按住拖"的分界纯属手感 —— 有人手抖、有人习惯长按，
// 写死 6px 就只能改代码重编译。

// 拖拽落点：光标压在哪一行、该行的上半还是下半 → 插入线位置。
//

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

    // 【统一判空，别在每个分支各写一遍】ref 由 SetWindowSubclass 带进来，理论上
    // 恒为有效 &st；但子类数据一旦被复用/窗口在销毁途中仍收到消息，解引用就是
    // 崩溃。以前只有 WM_CHAR 判了，其余分支（MOUSEMOVE/CAPTURECHANGED/KEYDOWN/
    // LBUTTONUP）直接解引用 —— 防御口径不一致，等于只有一条路有保护。
    // 放在 switch 之前：漏消息比崩掉好（DefWindowProc 会兜住）。
    if (!st) return DefSubclassProc(h, m, w, l);

    switch (m) {
    case WM_CHAR: {
        // 只吃可打印字符。控制字符（Ctrl 组合键产生的 1..26）交给下面，
        // 它们属于快捷键而不是"定位文本"。
        if (w < L' ' || w == 0x7F) break;
        const ULONGLONG now = GetTickCount64();
        // 【间隔判定用 GetTickCount64 而不是"字符相同就重置"】用户敲 "ss" 想在
        // 两个 s 开头的条目间轮换，与"打一个两字前缀 ss..."是同一串按键 ——
        // 只有时间能区分意图。默认 900ms 与系统 type-ahead 的量级相当，
        // 具体值来自设置 popupTypeAheadMs（慢打字者可自行调大）。
        const ULONGLONG resetMs = static_cast<ULONGLONG>(Config::Get().s().popupTypeAheadMs);
        if (now - st->typeTick > resetMs) st->typeBuf.clear();
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
        const int mx = GET_X_LPARAM(l);
        const int my = GET_Y_LPARAM(l);
        // 拖拽中：只算落点。hover 高亮此时会让位给插入线 ——
        // 两个"当前位置"提示同时出现，用户不知道看哪个。
        if (st->dragging) {
            UpdateDragPosition(st, h, mx, my);
            return 0;
        }
        // 按下后移动超阈值 = 起拖。阈值来自设置 popupDragThreshold（默认 6px，
        // 与书签栏顶层同口径）：太小会把"点一下选行"误判成拖动，太大则拖不动。
        // 每次移动现读 —— 它是纯手感值，改完下一次拖就生效。
        if (st->dragArm && !st->dragging) {
            const int dx = mx - st->dragStart.x;
            const int dy = my - st->dragStart.y;
            const int th = Config::Get().s().popupDragThreshold;
            if (dx * dx + dy * dy > th * th) {
                st->dragging = true;
                UpdateDragPosition(st, h, mx, my);
                return 0;
            }
        }
        int idx = ItemFromPoint(h, mx, my);
        if (idx >= RowCount(st)) idx = -1;
        if (RowInert(st, idx)) idx = -1;
        int newHotExp = -1;
        if (st->treeMode && idx >= 0) {
            RECT irc;
            if (SendMessageW(h, LB_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&irc)) >= 0) {
                RECT er = PopupExpanderRect(irc, *st->theme);
                POINT pt{mx, my};
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
        ClearDragOutPreview(st);
        st->dragArm = st->dragging = false;
        st->dragFrom = st->dropIndex = -1;
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
        // 左键：可能起一次行内拖拽（超阈值才算）。右键不起 —— 右键是"另起
        // 一次操作"（见 WM_RBUTTONUP），不是拖。
        if (m == WM_LBUTTONDOWN && st->reorder.onReorder) {
            const int row = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (row >= 0 && row < RowCount(st) && !RowInert(st, row) &&
                (!st->reorder.canDrag || st->reorder.canDrag(row))) {
                st->dragArm = true;
                st->dragging = false;
                st->dragFrom = row;
                st->dropIndex = -1;
                st->dragStart = pt;
            }
        }
        break;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT rc;
        GetClientRect(h, &rc);
        const bool upInside = PtInRect(&rc, pt) != FALSE;

        // 拖拽落点：**先于**"选中行"处理 —— 拖完松手是**移动**，不是选中。
        // 顺序反了的话，拖一下既改了顺序又顺手打开了那一项。
        if (st->dragging) {
            const int from = st->dragFrom;
            const int drop = st->dropIndex;
            const bool outside = st->dragOutside;
            if (outside && st->reorder.onDragOut) {
                POINT screen = pt;
                ClientToScreen(h, &screen);
                st->reorder.onDragOut(from, screen);
                ClearDragOutPreview(st);
                st->dragging = false;
                st->dragArm = false;
                st->dragFrom = -1;
                st->dropIndex = -1;
                st->done = true;
                if (GetCapture() == h) ReleaseCapture();
                PostMessageW(st->self, WM_CLOSE, 0, 0);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (outside) ClearDragOutPreview(st);
            st->dragging = false;
            st->dragArm = false;
            st->dragFrom = -1;
            st->dropIndex = -1;
            if (drop >= 0) {
                PopupApplyReorder(st, from, drop);
            } else if (from < 0 && st->reorder.allowExternalFrom) {
                // 外部移交 + 松手没压在有效落点上 = 加到分类末尾。
                // 保留"拖到分类 = 加入末尾"的旧语义：悬停弹层刚开就松手的那一下
                // 不是取消，是把书签放进这个分类。
                PopupApplyReorder(st, from, RowCount(st));
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        st->dragArm = false;

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
            // 行内右键：把命中的行号交回宿主，宿主在自己的循环里弹它的菜单
            // （右键菜单本身是模态的，不能在弹层的模态循环里再弹一层）。
            if (st->reorder.onRowRightClick) {
                const int row = ItemFromPoint(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (row >= 0 && row < RowCount(st) && !RowInert(st, row))
                    st->reorder.onRowRightClick(row);
            }
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
                RECT er = PopupExpanderRect(irc, *st->theme);
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
            ClearDragOutPreview(st);
            st->done = true;
            st->dragArm = st->dragging = false;
            st->dragFrom = st->dropIndex = -1;
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
