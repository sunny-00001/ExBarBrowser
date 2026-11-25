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

BookmarkDropSpot BookmarkBar::DropSpotAtClient(int x, int y) const {
    BookmarkDropSpot spot;
    if (!hwnd_ || !theme_) return spot;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // 溢出按钮右侧不是书签落点；与内部拖动原来的判定完全一致。
    const int margin = theme_->S(6);
    const int viewRight = overflow_
                              ? (rc.right - margin - ovfRc_.right + ovfRc_.left - theme_->S(4))
                              : rc.right;
    if (x < margin || x > viewRight || y < 0 || y >= rc.bottom) return spot;

    spot.valid = true;
    spot.index = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT& r = items_[i].rc;
        if (r.right <= r.left) continue;  // 被溢出折叠的项没有可见位置
        const int w = r.right - r.left;
        const int side = w / 4;
        if (items_[i].folder && x > r.left + side && x < r.right - side) {
            // 分类中间 50% = 放进该分类（与内部拖拽同语义）。
            spot.index = -1;
            spot.folderIndex = static_cast<int>(i);
            spot.folderId = FolderIdAt(static_cast<int>(i));
            break;
        }
        if (x > (r.left + r.right) / 2) spot.index = static_cast<int>(i) + 1;
    }
    return spot;
}

BookmarkDropSpot BookmarkBar::DropSpotAt(POINT screen) const {
    if (!hwnd_) return BookmarkDropSpot{};
    POINT cl = screen;
    ScreenToClient(hwnd_, &cl);
    return DropSpotAtClient(cl.x, cl.y);
}

std::wstring BookmarkBar::FolderIdAt(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return std::wstring();
    const Item& it = items_[static_cast<size_t>(index)];
    if (!it.folder) return std::wstring();  // 只有文件夹书签能作为落点容器
    return it.id;
}
