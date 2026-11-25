// 书签栏命中测试：客户区/屏幕点 → 可见项、图标格、溢出按钮、文件夹 id。
// 职责单一：只读 items_ / rc / overflow_，不写状态、不弹菜单、不导航。
// 与 bookmark_bar.cpp / bookmark_bar_draw.cpp 同属 BookmarkBar 类，头文件声明不变。
#include "bookmark_bar.h"

int BookmarkBar::HitItem(int x, int y) const {
    POINT pt{x, y};
    // 裁切线右侧是 » 按钮，不属于任何书签。判据必须按**点**判，不能只过滤
    // 项自己的矩形 —— 跨过裁切线的那一项仍然会盖在 » 按钮下面，于是点 »
    // 变成点它，溢出菜单永远打不开（标签栏踩过同一个坑，见 tab_strip.h 的
    // TabHitInView）。
    if (!BmkHitInView(x, viewRight_)) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!BmkInView(items_[i].rc, viewRight_)) continue;  // 滚出视野的项不命中
        if (PtInRect(&items_[i].rc, pt)) return static_cast<int>(i);
    }
    return -1;
}

int BookmarkBar::HitIcon(int x, int y) const {
    POINT pt{x, y};
    if (!BmkHitInView(x, viewRight_)) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!BmkInView(items_[i].iconRc, viewRight_)) continue;
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
    // 裁切线（» 按钮的左邻）之外不是书签落点。
    if (x < 0 || x > viewRight_ || y < 0 || y >= rc.bottom) return spot;

    spot.valid = true;
    spot.index = 0;
    bool firstVisible = true;
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT& r = items_[i].rc;
        if (!BmkInView(r, viewRight_)) continue;  // 滚出视野的项没有可见位置
        // 可见项可能只是**中间一段**（滚过去之后）。插入线的基准必须是
        // 第一个可见项的下标，不是 0 —— 否则落在中间也会插到最前面去。
        if (firstVisible) {
            spot.index = static_cast<int>(i);
            firstVisible = false;
        }
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
