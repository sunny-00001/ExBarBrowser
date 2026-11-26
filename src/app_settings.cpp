// 设置对话框的入口接线。
//
// 【为什么单独一个文件】App 里与设置有关的动作只有"打开对话框"这一件，
// 而对话框本身（模型 / 布局 / 绘制 / 交互）住在 settings_dialog.* 里。
// 把接线也塞回那个文件会让设置相关代码散在两处；单开一个文件，
// "App 到底怎么打开设置的"这个问题只有一个答案可查。
//
// 【为什么不再传锚点】旧的面板是弹出菜单，必须给一个屏幕坐标（否则贴在
// 鼠标右下角）。设置对话框是**独立窗口**：有自己的位置、记忆大小，
// 而且是非模态的 —— 用户可以一边开着它一边改主窗口。给一个锚点反而
// 把它拖回"弹层"的心智模型。
#include "app.h"

#include "settings_bounds.h"   // kHistoryDepthMax：裁剪导航深度上界与注册表同源

#include <algorithm>

void App::ShowSettingsDialog() {
    // 主题对象在 App 里，地址终身有效：settingsDialog_ 存的是指针，
    // ApplyTheme 重新 Reload 的是**同一份** Theme，对话框 ReloadTheme
    // 后立刻拿到新颜色 —— 不会出现"对话框用的是旧主题"。
    if (!settingsDialog_.Create(hwnd_, theme_)) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    settingsDialog_.Show();
}

void App::ShowHotkeyManager() {
    // 与设置对话框同一套接线：Create 幂等，主题与路由都由 App 持有，
    // 所以面板拿到的永远是当前那一份（不会出现"面板里的键位是启动时的快照"）。
    if (!hotkeyManager_.Create(hwnd_, theme_, input_)) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    hotkeyManager_.Show();
}

void App::ApplyBarHeights() {
    const Settings& s = Config::Get().s();
    // Theme 保存的是 96 DPI 逻辑值；真正缩放由 Theme::S 在每次 barXxx() 里做。
    theme_.SetBarHeights(s.tabBarHeight, s.navBarHeight, s.bookmarkBarHeight, s.statusBarHeight);
}

void App::ApplyHistoryLimits() {
    Settings& s = Config::Get().mutableSettings();

    // 最近访问 / 搜索历史：上限由 config 侧的 AddRecent / AddSearchHistory 在追加时
    // 夹取，这里再裁一次是为了"把上限调小"立刻可见 —— 否则下拉里仍挂着旧条目，
    // 用户会以为设置没生效。裁剪顺序与追加一致（front = 最新，从尾部截断）。
    const int recentLimit = std::max(1, std::min(200, s.recentLimit));
    if (static_cast<int>(s.recentPaths.size()) > recentLimit)
        s.recentPaths.resize(static_cast<size_t>(recentLimit));
    const int searchLimit = std::max(1, std::min(100, s.searchLimit));
    if (static_cast<int>(s.searchHistory.size()) > searchLimit)
        s.searchHistory.resize(static_cast<size_t>(searchLimit));

    // 关闭历史：上限的真源在 TabSet（PushClosed / SetClosed 都用它）。
    tabSet_.SetClosedLimit(std::max(0, std::min(100, s.closedTabLimit)));

    // 导航历史深度：运行中改 historyDepth 时，推给每个标签的每个窗格并立即裁剪。
    // 新窗格由 App::PaneAt 在创建时注入，这里覆盖已经开着的那批。
    const size_t depth = static_cast<size_t>(
        std::max(0, std::min(kHistoryDepthMax, s.historyDepth)));
    for (const auto& kv : tabs_) {
        const Tab* t = kv.second.get();
        if (!t) continue;
        for (const auto& pane : t->panes)
            if (pane) pane->nav.SetMaxEntries(depth);
    }
}
