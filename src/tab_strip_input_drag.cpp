// 标签栏拖拽的"松手提交"：把拖动态的落点翻译成一条结构化命令。
//
// 【为什么单独成文件】WM_LBUTTONUP 的其余部分只做"命中 → 组件默认行为/鼠标规则"，
// 而这一段是"落点 → 命令"，两种语义。拆开后 tab_strip_input.cpp 不再顶着
// 500 行硬上限，后续加拖动光标/取消路径也有地方放注释。
//
// 【顺序是这一段全部的价值】ReleaseCapture 会同步派发 WM_CAPTURECHANGED，
// 那条分支会清掉 dragIdx_/dropIdx_；所以所有要发的值必须先读进局部变量，
// 再释放捕获。反过来写就是"拖了没反应"且没有任何报错。
#include "tab_strip.h"

#include "common.h"   // WM_APP_TAB_MOVED / WM_APP_TAB_TEAROFF

bool TabStrip::HandleDragCommit(HWND h) {
    // 【与 BookmarkBar 同口径：先提交拖拽，再问鼠标规则】调用方保证 dragIdx_>=0；
    // 规则层没有理由在松手这一刻接管一个已经明确的拖拽意图。
    const int from = dragIdx_;
    const int to = dropIdx_;
    const int fromId = dragId_;   // 按下时定格，不按当前 from 反查（入队期间集合会变）
    // tear-off：脱离栏体松手 = "移到新窗口"（走命令，与菜单/热键同一实现）。
    // 判定用 tearOff_ 而不是现算坐标 —— 松手瞬间光标可能已回到栏内，
    // 但用户看到的提示条还是"松开：移到新窗口"，行为必须与提示一致。
    const bool tear = tearOff_ && dragging_;
    const bool moved = dragging_ && to >= 0 && to != from;
    StopAutoScroll();   // 拖到边缘滚着松手：先停表，再提交移动
    dragIdx_ = -1;
    dragging_ = false;
    dropIdx_ = -1;
    tearOff_ = false;
    dragId_ = -1;
    ReleaseCapture();
    // 【消息里传 id，不传下标】见 NotifyClose / NotifySelect 的注释：
    // 消息入队后集合可能已被别的消息改动，下标会指错标签。
    // to（落点）仍是下标 —— 那是空间语义，用户预期就是"放到那个位置"。
    if (tear && fromId >= 0) {
        PostMessageW(parent_, WM_APP_TAB_TEAROFF, static_cast<WPARAM>(fromId), 0);
    } else if (moved && fromId >= 0) {
        PostMessageW(parent_, WM_APP_TAB_MOVED, static_cast<WPARAM>(fromId),
                     static_cast<LPARAM>(to));
    }
    InvalidateRect(h, nullptr, FALSE);
    RebuildTooltip(h);   // 拖动开始时 Clear 过：松手恢复 tooltip
    return true;
}
