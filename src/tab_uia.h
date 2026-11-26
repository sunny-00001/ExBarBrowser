// 自绘栏的 UIA（UI Automation）桥 —— 标签栏与书签栏共用。
//
// 自绘窗口默认对读屏软件是"一整块空白"：Narrator/检查器拿不到条目名、
// 选中态，也没法用 UIA 选中某个条目。这里提供最小但完整的 provider：
//   · 根：Tab 容器（ControlType=Tab），子项是各条目；
//   · 项：Name/选中/边界/Invoke/SelectionItem，选中即走到该条目。
//
// 只做"读 + 选中"，不暴露关闭/拖动等复合动作 —— 那些走命令与菜单，
// 可访问性不该成为第二套操作模型。
//
// 【泛化】provider 本体与栏的具体类型无关（数据全走 kMsg* 消息回 UI 线程），
// 两种栏的差别只有 traits：根名称 / ControlType / automationId 前缀 /
// 宿主类指针。谁接入，谁在自己的头文件里给一份 Traits（见下方两个别名）。
#pragma once
#include "common.h"

#include <string>

class TabStrip;
class BookmarkBar;

namespace uia {

// 跨线程查询用的消息（provider → UI 线程）。同一进程、SendMessage 同步，
// 所以 ItemInfo 里的 std::wstring 在返回前一直有效。
// 消息号是"窗口类私有语义"：两栏都用 0x51..0x53 没有冲突（不同 hwnd）。
constexpr UINT kMsgCount = WM_APP + 0x51;   // 返回条目数
constexpr UINT kMsgItem = WM_APP + 0x52;    // wParam=index, lParam=ItemInfo*
constexpr UINT kMsgSelect = WM_APP + 0x53;  // wParam=index（UIA 选中）

struct ItemInfo {
    std::wstring name;
    std::wstring path;
    RECT rect{};          // 客户区坐标
    bool selected = false;
};

// 两种栏的差异面：root 名称/控件类型、automationId 前缀、宿主类指针。
// provider 只认这份 traits，不知道后面站着的是标签还是书签。
struct Traits {
    const wchar_t* rootName = nullptr;     // 根元素的可读名（"标签栏"/"书签栏"）
    int rootControlType = 0;               // UIA_TabControlTypeId（50018）
    const wchar_t* automationPrefix = nullptr;  // 项 id 前缀（"tab."/"bmk."）
    void* owner = nullptr;                 // 宿主类指针（只透传，provider 不解引用）
};

// Traits 的具体值：TabTraitsFor 在 tab_uia.cpp；书签栏的 TraitsFor 在
// bookmark_bar_uia.cpp（namespace bmuia，两栏各自的类型归属更清晰）。
Traits TabTraitsFor(TabStrip* s);

// WM_GETOBJECT 入口：lParam == OBJID_CLIENT 时返回 raw element provider。
LRESULT HandleGetObject(const Traits& traits, HWND hwnd, WPARAM w, LPARAM l);

// 窗口销毁时释放该 hwnd 的根 provider（UIA 客户端仍持有的引用由 COM 计数管）。
void Release(HWND hwnd);

}  // namespace uia
