// 弹层窗口外壳：注册窗口类、PopupProc 消息分发、RunModal 建窗与模态循环，
// 两个公开 Show* 入口。输入状态机在 popup_list_input.cpp，树/几何在 popup_list_tree.cpp。
#include "popup_list_internal.h"

#include "config.h"           // 弹层手感来自设置（键入间隔 / 树弹层行数）
#include "modal.h"        // modal::Scope：登记模态层（AI 泵据此避让）
#include "platform_win.h"
#include "popup_list_draw.h"   // 行绘制 + 宽度计算（独立职责，见该文件头）
#include "popup_toggle.h"

#include "ui_draw.h"

namespace {

using id::kPopupClass;

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
        // 行绘制在独立 TU（popup_list_draw.cpp）：这里只把状态机里的东西
        // 整理成一份只读快照递过去，不让绘制逻辑认识捕获/type-ahead/懒加载。
        PopupDrawParams dp;
        dp.treeMode = st->treeMode;
        dp.rows = st->rows;
        dp.tree = st->treeMode ? &st->tree : nullptr;
        dp.visible = st->treeMode ? &st->visible : nullptr;
        dp.theme = st->theme;
        dp.iconS = st->iconS;
        dp.hot = st->hot;
        dp.curSel = st->curSel;
        dp.hotExpander = st->hotExpander;
        dp.checkCol = st->checkCol;
        // 拖拽插入线：画在"插到这一行之前"的那一行上（见 popup_list_draw.cpp）。
        dp.dropIndex = st->dropIndex;
        PopupDrawItem(dis->hDC, dis, dp);
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
    // 外部移交的拖拽：进场即在拖（没有"按下"这一步可等），起始行号 -1 = 外部项。
    // 后续 WM_MOUSEMOVE 直接走 dragging 分支画插入线，UP 按落点提交或加入末尾。
    if (st.reorder.allowExternalFrom) {
        st.dragging = true;
        st.dragFrom = -1;
        st.dropIndex = -1;
    }
    popup::NoteOpened(st.toggleKey.c_str());

    // 对齐 mod 19298-19301：GetMessage 返回 -1 是错误（不能继续循环），
    // 返回 0 是 WM_QUIT（必须原样 PostQuitMessage 交还宿主）。
    // modal::Scope：把这段循环登记成"模态层" —— 模态期间不派发 AI 命令
    // （见 modal.h 里 Depth() 的说明）。
    modal::Scope modalScope;
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
                  int maxRows, const wchar_t* toggleKey, PopupInitial init,
                  PopupReorder reorder) {
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
    st.checkCol = HasCheckedRow(rows);
    st.selectRow = init.selectRow;
    st.scrollTo = init.scrollTo;
    st.reorder = std::move(reorder);
    // 宿主给了 rebuildRows → 行内容归弹层持有（重排后要就地刷新）。
    // 否则弹层只借用调用方的只读数组，不复制也不改。
    if (st.reorder.rebuildRows) {
        st.rowsOwned = rows;
        st.rows = &st.rowsOwned;
        st.ownsRows = true;
    }

    int width = PopupMeasureWidth(owner, theme, &rows, nullptr, st.iconS, exactWidth, minWidthPx);
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

    int width = PopupMeasureWidth(owner, theme, nullptr, &st.tree, st.iconS, false, minWidthPx);
    int topCount = 0;
    for (const auto& n : st.tree) if (n.parent < 0) ++topCount;
    // 树弹层可见行数来自设置 popupMaxRows（与 ResizeToFit 同一口径）。
    int visible = std::min<int>(topCount, Config::Get().s().popupMaxRows);
    int height = visible * st.itemH + 2;  // 树模式无分隔行，恒为 itemH

    RunModal(st, owner, screen, width, height);
    std::wstring result = st.treePicked;
    ClearTree(st.tree);   // 节点 icon 归 ico::Cached，弹层只清结构
    return result;
}
