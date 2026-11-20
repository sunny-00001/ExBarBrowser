// 布局与主题（App 的编排分片）：四条栏的高度/显隐、视图区回收、DPI 与深浅色切换。
//
// 改这里只有一个理由：客户区的空间怎么分。栏内部怎么画不在此处（各自 ReloadTheme）。
#include "app.h"

// ---------------------------------------------------------------- 布局
void App::Layout() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const Settings& s = Config::Get().s();
    const int tabH = s.showTab ? theme_.S(kTabBarHeightDp) : 0;
    const int navH = s.showNav ? theme_.S(30) : 0;
    const int bmkH = s.showBookmark ? theme_.S(26) : 0;
    const int stH = s.showStatus ? theme_.S(24) : 0;

    int y = 0;
    if (s.showTab) {
        MoveWindow(tabStrip_.hwnd(), 0, y, rc.right, tabH, TRUE);
        ShowWindow(tabStrip_.hwnd(), SW_SHOW);
        y += tabH;
    } else {
        ShowWindow(tabStrip_.hwnd(), SW_HIDE);
    }
    if (s.showNav) {
        MoveWindow(navBar_.hwnd(), 0, y, rc.right, navH, TRUE);
        ShowWindow(navBar_.hwnd(), SW_SHOW);
        y += navH;
    } else {
        ShowWindow(navBar_.hwnd(), SW_HIDE);
    }
    if (s.showBookmark) {
        MoveWindow(bookmarkBar_.hwnd(), 0, y, rc.right, bmkH, TRUE);
        ShowWindow(bookmarkBar_.hwnd(), SW_SHOW);
        y += bmkH;
    } else {
        ShowWindow(bookmarkBar_.hwnd(), SW_HIDE);
    }

    int viewH = rc.bottom - y - stH;
    if (viewH < 0) viewH = 0;
    MoveWindow(viewArea_, 0, y, rc.right, viewH, TRUE);

    if (s.showStatus) {
        MoveWindow(statusBar_.hwnd(), 0, rc.bottom - stH, rc.right, stH, TRUE);
        ShowWindow(statusBar_.hwnd(), SW_SHOW);
    } else {
        ShowWindow(statusBar_.hwnd(), SW_HIDE);
    }

    RECT inner{0, 0, rc.right, viewH};
    for (auto& kv : tabs_) kv.second->view->Resize(inner);
}

void App::ApplyTheme(int dpiOverride) {
    if (!hwnd_) return;
    theme_.Reload(hwnd_, dpiOverride);
    ApplyProcessDarkMode(theme_.dark);
    theme_.ApplyToViewArea(viewArea_);
    ApplyDarkTitleBar(hwnd_, theme_.dark);
    tabStrip_.ReloadTheme();
    navBar_.ReloadTheme();
    bookmarkBar_.ReloadTheme();
    statusBar_.ReloadTheme();
    for (auto& kv : tabs_) kv.second->view->ApplyTheme();
    InvalidateRect(hwnd_, nullptr, TRUE);
    Layout();
}
