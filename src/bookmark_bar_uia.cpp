// 书签栏的 UIA 接入：Traits + HandleGetObject 的转发。
//
// 【为什么只有这么短】provider 本体在 tab_uia.cpp（与标签栏共用，泛化成
// Traits 参数化）；书签栏这边只需要：
//   1. 一份 Traits（根名/automationId 前缀不同）；
//   2. BookmarkTraits 的定义（头文件里只有 inline 声明，类成员访问放这里）。
// 消息分支（kMsgCount/kMsgItem/kMsgSelect）在 bookmark_bar.cpp 的 Handle 里，
// 与标签栏同一套写法 —— 两栏的 UIA 行为天然一致，不存在第二套 provider。
#include "bookmark_bar.h"

namespace bmuia {

uia::Traits TraitsFor(BookmarkBar* b) {
    uia::Traits t;
    t.rootName = L"书签栏";
    t.rootControlType = 50018;          // UIA_TabControlTypeId（列表式条目容器）
    t.automationPrefix = L"bmk.";
    t.owner = b;
    return t;
}

LRESULT HandleGetObject(BookmarkBar* b, HWND hwnd, WPARAM w, LPARAM l) {
    return uia::HandleGetObject(TraitsFor(b), hwnd, w, l);
}

}  // namespace bmuia
