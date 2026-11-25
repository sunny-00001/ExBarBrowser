// 导航栏命中测试：只回答"这个点落在哪块矩形"，不改任何状态。
//
// 【为什么和布局同源】这里读的全是 NavBar::Layout() 产出的矩形
// （b.rc / b.arrow / moreRc_ / dropRc_ / segRects_ / chevRects_）。命中与绘制
// 各算一遍矩形必然漂移，漂移的表现是"点后退却打开了前进历史"。
//
// 【NavTargetAt 是鼠标规则的翻译层】把命中结果翻成 MT_NAV / MT_ADDRESS，
// 交给 MouseSink；它不决定动作，动作仍由 nav_bar.cpp 的消息分发执行。
#include "nav_bar.h"

#include "mouse_rules.h"

int NavBar::HitButton(int x, int y) const {
    POINT pt{x, y};
    for (const auto& b : buttons_)
        if (PtInRect(&b.rc, pt)) return b.id;
    return 0;
}

bool NavBar::HitArrow(int x, int y, int* id) const {
    POINT pt{x, y};
    for (const auto& b : buttons_) {
        if (b.hasArrow && PtInRect(&b.arrow, pt)) {
            *id = b.id;
            return true;
        }
    }
    return false;
}

bool NavBar::HitMore(int x, int y) const {
    POINT pt{x, y};
    return moreRc_.right > moreRc_.left && PtInRect(&moreRc_, pt);
}

bool NavBar::HitDrop(int x, int y) const {
    POINT pt{x, y};
    return PtInRect(&dropRc_, pt) != FALSE;
}

int NavBar::HitSeg(int x, int y) const {
    POINT pt{x, y};
    for (size_t k = 0; k < segRects_.size(); ++k)
        if (PtInRect(&segRects_[k], pt)) return firstShown_ + static_cast<int>(k);
    return -1;
}

int NavBar::NavTargetAt(int x, int y) const {
    int arrowId = 0;
    if (HitArrow(x, y, &arrowId) || HitButton(x, y) != 0 || HitMore(x, y) || HitDrop(x, y))
        return MT_NAV;
    if (HitSeg(x, y) >= 0 || HitChev(x, y) >= 0) return MT_ADDRESS;
    if (x >= fieldRc_.left) return MT_ADDRESS;
    return MT_NAV;
}

int NavBar::HitChev(int x, int y) const {
    POINT pt{x, y};
    for (size_t k = 0; k < chevRects_.size(); ++k)
        if (PtInRect(&chevRects_[k], pt)) return firstShown_ + static_cast<int>(k);
    return -1;
}
