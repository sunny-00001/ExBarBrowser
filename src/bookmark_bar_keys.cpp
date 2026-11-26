// 书签栏的键盘导航：方向键 / Home / End / 回车 / 菜单键。
//
// 【为什么单独成文件】bookmark_bar.cpp 的 Handle 里塞着绘制、焦点、UIA、拖拽、
// 菜单、键盘六类消息；键盘这一族自成一体（一个"焦点项游标 + 保证可见"的语义），
// 拆出来读的人才不用在几百行 switch 里找那几十行。
//
// 【无障碍口径】图标能做的，键盘也要能做到 —— 所以菜单键（VK_APPS / Shift+F10）
// 与右键等价，方向键走遍**全部**项（不只可见项）。
#include "bookmark_bar.h"

#include "common.h"

// 返回 0 = 已处理；没认领的键交回 DefWindowProcW。
LRESULT BookmarkBar::HandleKeyDown(HWND h, WPARAM w, LPARAM l) {
    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        if (w == VK_ESCAPE && refocus_) refocus_();
        return 0;
    }
    if (w == VK_ESCAPE) {
        kbdIdx_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        if (refocus_) refocus_();   // 焦点还给视图，Delete / type-ahead 才可用
        return 0;
    }
    if (w == VK_LEFT || w == VK_RIGHT) {
        // 走遍**全部**项。以前放不下的项被折叠、栏上没有位置，
        // 键盘只能停在折叠项之前，看它们得用 » 菜单 —— 键盘用户等于摸不到。
        // 现在项都有位置，方向键走过去时顺手把栏滚到那一项（EnsureVisible），
        // 焦点框不会停在看不见的地方。
        const int step = (w == VK_RIGHT) ? 1 : -1;
        const int cur = kbdIdx_ >= 0 ? kbdIdx_ : (step > 0 ? -1 : n);
        int next = cur + step;
        if (next < 0) next = 0;
        if (next >= n) next = n - 1;
        kbdIdx_ = next;
        EnsureVisible(next);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (w == VK_HOME || w == VK_END) {
        kbdIdx_ = (w == VK_HOME) ? 0 : n - 1;
        EnsureVisible(kbdIdx_);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (w == VK_RETURN || w == VK_SPACE) {
        if (kbdIdx_ >= 0 && kbdIdx_ < n) {
            EnsureVisible(kbdIdx_);   // 键盘触发必可见（锚点取 rc，同 WM_CONTEXTMENU）
            POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                     items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
            ClientToScreen(h, &at);
            if (items_[static_cast<size_t>(kbdIdx_)].folder)
                ShowFolderMenu(kbdIdx_, at);
            else
                Navigate(kbdIdx_);
        }
        return 0;
    }
    if (w == VK_APPS || (w == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
        // 无障碍要求：图标/鼠标能做的，键盘也要能做到（等同右键）。
        if (kbdIdx_ >= 0 && kbdIdx_ < n) {
            EnsureVisible(kbdIdx_);
            POINT at{items_[static_cast<size_t>(kbdIdx_)].rc.left,
                     items_[static_cast<size_t>(kbdIdx_)].rc.bottom};
            ClientToScreen(h, &at);
            ShowItemMenu(kbdIdx_, at);
        }
        return 0;
    }
    return DefWindowProcW(h, WM_KEYDOWN, w, l);
}
