// 书签栏命中测试：客户区/屏幕点 → 可见项、图标格、溢出按钮、文件夹 id。
// 职责单一：只读 items_ / rc / overflow_，不写状态、不弹菜单、不导航。
// 与 bookmark_bar.cpp / bookmark_bar_draw.cpp 同属 BookmarkBar 类，头文件声明不变。
#include "bookmark_bar.h"

int BookmarkBar::HitItem(int x, int y) const {
    POINT pt{x, y};
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rc.right <= items_[i].rc.left) continue;
        if (PtInRect(&items_[i].rc, pt)) return static_cast<int>(i);
    }
    return -1;
}

int BookmarkBar::HitIcon(int x, int y) const {
    POINT pt{x, y};
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].iconRc.right <= items_[i].iconRc.left) continue;
        if (PtInRect(&items_[i].iconRc, pt)) return static_cast<int>(i);
    }
    return -1;
}

bool BookmarkBar::HitOverflow(int x, int y) const {
    if (!overflow_) return false;
    POINT pt{x, y};
    return PtInRect(&ovfRc_, pt) != FALSE;
}

int BookmarkBar::FolderItemAt(POINT screen) const {
    POINT pt = screen;
    if (!hwnd_) return -1;
    POINT cl = pt;
    ScreenToClient(hwnd_, &cl);
    return HitItem(cl.x, cl.y);
}

std::wstring BookmarkBar::FolderIdAt(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return std::wstring();
    const Item& it = items_[static_cast<size_t>(index)];
    if (!it.folder) return std::wstring();  // 只有文件夹书签能作为落点容器
    return it.id;
}
