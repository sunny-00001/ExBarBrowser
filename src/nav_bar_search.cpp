// 导航栏搜索框（从 nav_bar.cpp 拆出，保持单文件 <=500 行）。
//
// 形态对齐 ExBar mod：左带（放大镜 + 角上的小箭头，点开「搜索范围」）、
// 中间输入条、右带（箭头，点开「搜索历史」）。mod 的几何实测值见
// nav_metrics.h（SEARCH_W=200 / btnZone=26 / filterW=24 / rightPad=4）。
//
// 【搜索框常驻】以前它只在 Ctrl+F 时出现，平时把宽度还给地址框 ——
// 于是"搜索"这个每天都要用的入口反而要靠一个快捷键才找得到。
// 现在它在窄窗口下才整体收起（宽度判定在 Layout 一处完成）。
//
// 【弹层内容不在这里】范围与历史的真相在 Config::Settings 里，
// 导航栏不持有副本 —— 只算锚点并把点击转给 App（与"设置…""选择详细信息…"同一套路）。
#include "nav_bar.h"

#include "nav_metrics.h"
#include "platform_win.h"
#include "ui_draw.h"

namespace {

WNDPROC g_oldSearchProc = nullptr;

}  // namespace

LRESULT CALLBACK NavBar::SearchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = GetThis<NavBar>(GetParent(h));
    if (m == WM_KEYDOWN && self) {
        if (w == VK_RETURN) {
            self->CommitSearch();
            return 0;
        }
        if (w == VK_ESCAPE) {
            // Esc：清掉搜索词并退出搜索，把目录还原。
            // 不这么做的话"搜过一次"就再也没有回到原目录的路径了
            // （地址框显示的还是原路径，但列表是筛选后的结果）。
            self->ClearSearch();
            return 0;
        }
        if (w == VK_TAB) {
            // Tab 离开输入框，去文件列表那边（搜索框常驻，不用清词）。
            // 不这么处理的话 Tab 在搜索框里是死键 —— 原生 EDIT 没有 tabstop 处理。
            if (self->hwnd())
                PostMessageW(GetAncestor(self->hwnd(), GA_ROOT), WM_APP_REFOCUS, 0, 0);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == L'\t')) return 0;
    return CallWindowProcW(g_oldSearchProc, h, m, w, l);
}

void NavBar::SearchCreate() {
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10,
                              10, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!search_) return;
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_->font), TRUE);
    win::ThemeControl(search_, theme_->dark, L"CFD");
    // 提示文本用 EM_SETCUEBANNER：不占内容，用户一打字就消失（不是真值）。
    SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索（回车执行，Esc 取消）"));
    // 子类化以接 Enter/Esc —— 与地址框同一套做法（原生 EDIT 不会替我们处理）。
    g_oldSearchProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(search_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchProc)));
    // 显示与否由 Layout 决定（窄窗口要收起），这里先藏起来免得闪一下。
    ShowWindow(search_, SW_HIDE);
}

void NavBar::LayoutSearch(int fieldRight, int fy, int fieldH, int lineH) {
    if (!search_) return;

    // 宽度判定在 Layout 一处完成：那里算好了给搜索框留了多宽，
    // 这里只负责摆放（两边各算一次宽度必然对不上）。
    const int boxLeft = fieldRc_.right + theme_->S(kGap96);
    if (boxLeft >= fieldRight) {
        // 窗口太窄：整体收起（地址框优先 —— 那是每天用几十次的那个）。
        searchShown_ = false;
        searchRc_ = {};
        searchMenuRc_ = {};
        searchHistRc_ = {};
        ShowWindow(search_, SW_HIDE);
        return;
    }
    searchRc_ = {boxLeft, fy, fieldRight, fy + fieldH};
    searchShown_ = true;

    const int inset = theme_->S(2);
    const int btnZone = theme_->S(kSearchBtnZone96);
    const int histZone = theme_->S(kSearchHistZone96);
    const int gap = theme_->S(kSearchGap96);
    // 左带：放大镜（点开「搜索范围」）
    searchMenuRc_ = {searchRc_.left + inset, searchRc_.top + inset,
                      searchRc_.left + btnZone, searchRc_.bottom - inset};
    // 右带：历史下拉（点开「搜索历史」）
    searchHistRc_ = {searchRc_.right - inset - histZone, searchRc_.top + inset,
                      searchRc_.right - inset, searchRc_.bottom - inset};

    // 输入条：夹在两条带之间。行高取 Layout 的实测值（与地址框同源），
    // 再按同一居中原语放进框里 —— 两个框并排时字基线才齐、选中才不跳。
    const RECT lineRc = ui::CenteredLineRect(searchRc_, lineH);
    const int editLeft = searchMenuRc_.right + theme_->S(2);
    const int editRight = searchHistRc_.left - gap;
    const int eW = editRight - editLeft;
    ShowWindow(search_, SW_SHOW);
    MoveWindow(search_, editLeft, lineRc.top, eW > 0 ? eW : 0,
               lineRc.bottom - lineRc.top, TRUE);
}

void NavBar::CommitSearch() {
    if (!search_) return;
    const int n = GetWindowTextLengthW(search_);
    std::wstring q(static_cast<size_t>(n) + 1, L'\0');
    if (n > 0) GetWindowTextW(search_, q.data(), n + 1);
    q.resize(static_cast<size_t>(n));
    searchText_ = str::Trim(q);
    if (onSearch_) onSearch_(searchText_);
    // 回车后焦点还给视图：搜索结果是文件列表，下一步操作（方向键/Delete）
    // 应该在列表上，而不是继续留在输入框里。
    if (parent_) PostMessageW(GetAncestor(hwnd_, GA_ROOT), WM_APP_REFOCUS, 0, 0);
}

void NavBar::ClearSearch() {
    if (!search_) return;
    SetWindowTextW(search_, L"");
    searchText_.clear();
    // 清空搜索 → 目录还原。这是 Esc 的语义，也是"框里空了但列表还是筛选结果"
    // 这种鬼状态的唯一出路。
    if (onSearch_) onSearch_(std::wstring());
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NavBar::FocusSearch() {
    if (!hwnd_ || !search_) return;
    // 搜索框常驻，Ctrl+F 只是把焦点送过去（并选中现有的词，便于改条件）。
    if (!searchText_.empty()) SetWindowTextW(search_, searchText_.c_str());
    SetFocus(search_);
    SendMessageW(search_, EM_SETSEL, 0, -1);
}

bool NavBar::IsSearching() const { return !searchText_.empty(); }

void NavBar::SetSearchText(const std::wstring& q) {
    searchText_ = q;
    if (!search_) return;
    SetWindowTextW(search_, q.c_str());
    SendMessageW(search_, EM_SETSEL, 0, -1);
}

bool NavBar::HitSearchMenu(int x, int y) const {
    if (!searchShown_) return false;
    POINT pt{x, y};
    return PtInRect(&searchMenuRc_, pt) != FALSE;
}

bool NavBar::HitSearchHist(int x, int y) const {
    if (!searchShown_) return false;
    POINT pt{x, y};
    return PtInRect(&searchHistRc_, pt) != FALSE;
}

void NavBar::ShowSearchScopeMenu() {
    if (!onSearchScope_ || !searchShown_) return;
    // 锚点取**搜索框**的左下角（不是带的位置）：弹层宽度 = 整框宽时，
    // 只有从左边缘起算才能左右边缘都与搜索框齐平。
    // exbar 的两个下拉都锚在 searchBoxRc.left / .bottom（exbar.cpp:19883、
    // :20056），这里与它一致 —— 两边锚点不同会让两个弹层左右错开。
    POINT at{searchRc_.left, searchRc_.bottom};
    ClientToScreen(hwnd_, &at);
    onSearchScope_(at, searchRc_.right - searchRc_.left);
}

void NavBar::ShowSearchHistoryMenu() {
    if (!onSearchHistory_ || !searchShown_) return;
    // 同上：锚点与宽度都取整个搜索框，不取右带。
    POINT at{searchRc_.left, searchRc_.bottom};
    ClientToScreen(hwnd_, &at);
    onSearchHistory_(at, searchRc_.right - searchRc_.left);
}
