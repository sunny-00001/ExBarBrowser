// 书签栏的滚轮：把放不下的书签滚进视野。
//
// 【为什么单独成文件】滚轮这条链路有三段：消息入口（bookmark_bar.cpp 的
// WM_MOUSEWHEEL 分支）、"规则优先 + 默认滚动"（这里）、宿主按光标位置转交
// （App 的消息循环，见 HandleWheelAt 的说明）。它有自己的边界条件，
// 挤进 bookmark_bar.cpp 会把那个文件顶过 500 行上限（它本来就只剩几十行余量）。
//
// 【为什么需要宿主转交】滚轮消息只发给**焦点窗口**。光标停在书签栏上、
// 焦点却在文件列表时，书签栏收不到 WM_MOUSEWHEEL —— 用户在栏上滚滚轮，
// 滚的却是背后的文件列表。宿主（App::Run 的消息循环）在派发之前按光标
// 位置转交过来，与序列弹层用的是同一手法（见 input_router_overlay.cpp）。
#include "bookmark_bar.h"

#include "config.h"       // 滚轮步进读设置 bookmarkWheelStepDp
#include "mouse_rules.h"

namespace {

// 滚轮一步的位移：来自设置 bookmarkWheelStepDp（默认 48 ≈ 三个字宽）。
// 以前是裸常量 —— 书签条目宽窄差异大，"一格跳过一条"与"一格挪一点点"
// 都有人要，写死就只能改代码重编译。每次滚动现读，改完立即生效。

}  // namespace

// 规则优先 → 默认横向滚动。返回 true = 已消费。
bool BookmarkBar::OnWheel(UINT msg, WPARAM w, LPARAM l) {
    // 滚轮消息的坐标是屏幕坐标；sink 的契约也是屏幕坐标，别再 ScreenToClient
    // 后传出去（传了客户区坐标，鼠标规则拿到的位置是错的）。
    const POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);

    // 光标不在本栏上就不接管：滚轮归焦点窗口。焦点在本栏、鼠标在别处时
    // 不该动这条栏（否则"焦点恰好在栏上"就会让文件列表滚不动）。
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return false;

    const int idx = HitItem(pt.x, pt.y);
    const int target =
        idx >= 0 ? (items_[static_cast<size_t>(idx)].folder ? MT_BOOKMARK_FOLDER : MT_BOOKMARK)
                 : (HitOverflow(pt.x, pt.y) ? MT_OVERFLOW : MT_BOOKMARK_EMPTY);
    // 例外：WM_MOUSEWHEEL / WM_MOUSEHWHEEL 的 button 位承载滚轮增量（common.h）。
    const int delta = GET_WHEEL_DELTA_WPARAM(w);
    // 先问鼠标规则：用户可能把"书签栏上滚滚轮"显式绑成别的动作。
    if (sink_ && sink_(target, msg, delta, CurrentMods(), screen)) return true;

    // 没有可滚的内容就不接管 —— 让滚轮继续给焦点窗口（比如背后的文件列表）。
    if (!overflow_) return false;

    const bool horizontal = (msg == WM_MOUSEHWHEEL);
    ScrollBy(BmkWheelScroll(delta,
                            theme_->S(Config::Get().s().bookmarkWheelStepDp), horizontal));
    return true;
}

// 宿主按光标位置把滚轮转交给本栏。返回 true = 已处理（宿主不再派发，
// 背后的文件视图不会跟着滚）。
bool BookmarkBar::HandleWheelAt(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!hwnd_ || !theme_ || !IsWindowVisible(hwnd_)) return false;
    if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) return false;
    const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // 光标不在本栏上就不接管（滚轮归焦点窗口）。
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return false;
    return OnWheel(msg, wParam, lParam);
}
