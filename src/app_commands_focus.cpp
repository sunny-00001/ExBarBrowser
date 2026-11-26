// 焦点域：焦点的**识别**（FocusPaneOf）与**循环**（F6 / Shift+F6）。
//
// 【为什么需要 F6】Explorer 的 F6 是"窗口元素间循环焦点"（地址栏 → 文件列表 →
// 导航树 → 状态栏）。本项目原先把 F6 给了"切下一个窗格"（view.pane.next），
// 于是单窗格时按 F6 什么也不会发生 —— 元素之间的焦点循环压根没有
// （EXPLORER-GAP-AUDIT §导航：「F6 在 Explorer 是窗口元素间循环焦点，
// 本仓四栏没有对应」）。
//
// 【链的顺序】地址栏 → 搜索框 → 文件列表 → 导航树 → 标签栏 → 书签栏 → 状态栏。
// 与 Explorer 一致的部分在前，本项目特有的栏在后。
// **双栏时文件列表是两站**（左视图、右视图各一站）—— Explorer 双栏下 F6
// 也是在两个窗格之间来回，所以这里跟它对齐，而不是把左右并成一个槽位。
//
// 【为什么不用"记住上次停在哪"】记位置的做法在用户自己点过某个栏之后就错位了
// （记住的位置与真实焦点不是一回事，按 F6 会跳到一个用户没去过的栏）。
// 所以每次都**从真实焦点反推**当前槽位：判定得出就从它的下一个开始；
// 判定不出（焦点在链外，比如在弹层里）就从链首开始。
//
// 【为什么用 FocusPaneOf 而不是自己判 hwnd】FocusPaneOf 是 state 快照里
// `focus` 字段的**唯一来源**（app_state.cpp 就调它）。自己再判一遍就多了一份
// 口径：两份迟早对不上，而对不上时断言 `focus=$changed` 会假红 ——
// 命令明明把焦点搬走了，断言却报"没变"。
//
// 【为什么这个文件叫 app_commands_*】命令处理器按域分片，扫描脚本只认
// app_commands*.cpp 这个前缀（tools/check_commands.py 的 HANDLERS_GLOB）。
// 焦点的识别与循环同属一个域，放在一起正好；拆开反而要跨文件传口径。
#include "app.h"

#include "app_commands.h"
#include "bookmark_bar.h"
#include "nav_bar.h"
#include "nav_pane.h"
#include "status_bar.h"
#include "tab_strip.h"

namespace {

// 焦点槽位。链的相对顺序在 BuildFocusChain 里定，这里只是标识。
enum class Slot { Address, Search, Files0, Files1, NavTree, Tabs, Bookmarks, Status };

// FocusPaneOf 报的串 → 槽位。返回 -1 = 不在链上（焦点在弹层/别的窗口）。
int SlotOfPaneName(const std::wstring& name) {
    if (name == L"address_bar") return static_cast<int>(Slot::Address);
    if (name == L"search_box") return static_cast<int>(Slot::Search);
    // 单栏时报 file_view，双栏时报 file_view_left / file_view_right。
    if (name == L"file_view" || name == L"file_view_left")
        return static_cast<int>(Slot::Files0);
    if (name == L"file_view_right") return static_cast<int>(Slot::Files1);
    if (name == L"nav_tree") return static_cast<int>(Slot::NavTree);
    if (name == L"tab_strip") return static_cast<int>(Slot::Tabs);
    if (name == L"bookmark_bar") return static_cast<int>(Slot::Bookmarks);
    if (name == L"status_bar") return static_cast<int>(Slot::Status);
    return -1;
}

// 当前的焦点链（按循环顺序）。双栏时文件列表是两站。
//
// 为什么是自由函数而不是 App 的成员：它只读 Config（双栏开没开），
// 不碰 App 的任何状态 —— 挂成成员就只是为了一个声明占 app.h 的行。
std::vector<int> BuildFocusChain() {
    std::vector<int> chain;
    chain.push_back(static_cast<int>(Slot::Address));
    chain.push_back(static_cast<int>(Slot::Search));
    chain.push_back(static_cast<int>(Slot::Files0));
    // 只在双栏时才有第二窗格。Pane::view 可能还没创建 —— 那一站到时跳过即可
    //（focusSlot 返回 false = 不可用，循环会继续往下找）。
    if (Config::Get().s().dualPane) chain.push_back(static_cast<int>(Slot::Files1));
    chain.push_back(static_cast<int>(Slot::NavTree));
    chain.push_back(static_cast<int>(Slot::Tabs));
    chain.push_back(static_cast<int>(Slot::Bookmarks));
    chain.push_back(static_cast<int>(Slot::Status));
    return chain;
}

}  // namespace

std::wstring App::FocusPaneOf(HWND focus) {
    if (!focus) return L"unknown";
    // 顺序 = 判定精度：自有窗口（书签栏/导航栏）用 hwnd 精确匹配，shell 提供的
    // 类名次之，最后才是视图容器子孙。都匹配不上就 unknown —— 猜错比不答更糟
    // （AI 会拿着错误焦点做决策）。
    if (bookmarkBar_.hwnd() &&
        (focus == bookmarkBar_.hwnd() || IsChild(bookmarkBar_.hwnd(), focus)))
        return L"bookmark_bar";
    if (navBar_.hwnd() && (focus == navBar_.hwnd() || IsChild(navBar_.hwnd(), focus))) {
        // 搜索框与地址框分开报：两者都在导航栏里，但"聚焦搜索框"与
        // "聚焦地址栏"是两件不同的事。合成一个值的话，
        // 调用方无法确认 win.focus.search 到底生效了没有。
        if (navBar_.searchHwnd() && focus == navBar_.searchHwnd()) return L"search_box";
        return L"address_bar";
    }
    // 标签栏与状态栏：F6 焦点循环要在这两栏之间停下来（Explorer 的 F6 也是
    // 在窗口元素间循环），所以它们必须是**可识别的焦点落点**。
    // 不加的话 FocusPaneOf 对这两处一律报 unknown —— 状态快照里看不出焦点
    // 在标签栏，AI 与断言都只能猜。
    if (tabStrip_.hwnd() && (focus == tabStrip_.hwnd() || IsChild(tabStrip_.hwnd(), focus)))
        return L"tab_strip";
    if (statusBar_.hwnd() && (focus == statusBar_.hwnd() || IsChild(statusBar_.hwnd(), focus)))
        return L"status_bar";
    wchar_t cls[64] = {};
    GetClassNameW(focus, cls, _countof(cls));
    if (lstrcmpW(cls, L"SysTreeView32") == 0) return L"nav_tree";
    if (Tab* t = Active()) {
        // 双栏下分辨焦点在左视图还是右视图：两个视图都归为 file_view 的话，
        // AI 无法知道当前命令会打到哪一栏，也就无法据焦点做决策。
        const bool dual = Config::Get().s().dualPane;
        for (int i = 0; i < 2; ++i) {
            const Pane* pane = t->panes[static_cast<size_t>(i)].get();
            if (!pane || !pane->view || !pane->view->hwnd()) continue;
            if (focus == pane->view->hwnd() || IsChild(pane->view->hwnd(), focus))
                return dual ? (i == 0 ? L"file_view_left" : L"file_view_right") : L"file_view";
        }
    }
    return L"unknown";
}

// 焦点循环。dir > 0 = 往后，dir < 0 = 往前。
void App::FocusCycle(int dir) {
    const std::vector<int> chain = BuildFocusChain();
    if (chain.empty()) return;
    const int step = dir >= 0 ? 1 : -1;

    // 把焦点放到某个槽位上。返回 false = 该槽位当前不可用（跳过它继续找下一个）。
    // 写成 lambda 而不是成员函数：它要用 App 的私有成员（各栏的句柄），
    // 但**只**在这里用一次 —— 挂成成员就只是为了在 app.h 里多占一行。
    auto focusSlot = [this](int slot) -> bool {
        switch (static_cast<Slot>(slot)) {
        case Slot::Address:
            // 地址框是**隐藏的 EDIT**：不 BeginEdit 就 SetFocus 是没用的
            //（焦点落在不可见的窗口上，GetFocus 拿得到但用户看不见光标）。
            if (!navBar_.addressHwnd()) return false;
            navBar_.FocusAddress();
            return true;
        case Slot::Search:
            if (!navBar_.searchHwnd()) return false;
            navBar_.FocusSearch();
            return true;
        case Slot::Files0:
        case Slot::Files1: {
            const int index = static_cast<Slot>(slot) == Slot::Files1 ? 1 : 0;
            Tab* t = Active();
            if (!t) return false;
            if (index == 1 && !EnsurePaneView(t, 1)) return false;
            Pane* p = PaneAt(t, index);
            if (!p || !p->view || !p->view->hwnd()) return false;
            // 直接把焦点给那一栏的 ShellView —— 活动窗格会由 OnFocusMaybeChanged
            // 跟着焦点走（"激活哪个视图就哪个响应"），这里不重复切。
            SetFocus(p->view->hwnd());
            return true;
        }
        case Slot::NavTree: {
            HWND tree = navPane_.treeWindow();
            if (!tree || !IsWindow(tree)) return false;
            SetFocus(tree);
            return true;
        }
        case Slot::Tabs:
            if (!tabStrip_.hwnd()) return false;
            SetFocus(tabStrip_.hwnd());
            return true;
        case Slot::Bookmarks:
            if (!bookmarkBar_.hwnd()) return false;
            bookmarkBar_.FocusBar();
            return true;
        case Slot::Status:
            if (!statusBar_.hwnd()) return false;
            SetFocus(statusBar_.hwnd());
            return true;
        default:
            return false;
        }
    };

    // 当前站在链上的第几站。判定不出（焦点在链外）→ -1。
    const int curSlot = SlotOfPaneName(FocusPaneOf(GetFocus()));
    int cur = -1;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (chain[i] == curSlot) {
            cur = static_cast<int>(i);
            break;
        }
    }

    // 起点：判定得出当前站 → 从它的下一站开始；
    // 判定不出 → 正向从头开始、反向从尾开始（首尾相接，绕一圈回到原处）。
    int idx;
    if (cur >= 0) {
        idx = cur;
    } else {
        idx = step > 0 ? -1 : static_cast<int>(chain.size());
    }

    const int n = static_cast<int>(chain.size());
    for (int i = 0; i < n; ++i) {
        idx = (idx + step + n) % n;
        if (focusSlot(chain[static_cast<size_t>(idx)])) return;
    }
}

void App::RegisterFocusCommands() {
    auto reg = cmd_reg::Register;
    // 断言 `focus=$changed`：期望值不是任何一个入参，而是"和刚才不一样"——
    // 焦点循环的下一站取决于当前焦点，静态写不出具体值，但"焦点确实搬了"
    // 是可以断言的（$changed 比的就是执行前后的 state.focus）。
    reg(CmdId::FocusNextElement, [this](const Args&) {
        FocusCycle(1);
        return true;
    });
    reg(CmdId::FocusPrevElement, [this](const Args&) {
        FocusCycle(-1);
        return true;
    });
}
