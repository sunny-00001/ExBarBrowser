// 标签栏的 UIA（UI Automation）桥。
//
// 自绘窗口默认对读屏软件是"一整块空白"：Narrator/检查器拿不到标签名、
// 选中态，也没法用 UIA 选中某个标签。这里提供最小但完整的 provider：
//   · 根：Tab 容器（ControlType=Tab），子项是各标签；
//   · 项：Name/选中/边界/Invoke/SelectionItem，选中即切标签。
//
// 只做"读 + 选中"，不暴露关闭/拖动等复合动作 —— 那些走命令与菜单，
// 可访问性不该成为第二套操作模型。
#pragma once
#include "common.h"

#include <string>

class TabStrip;

namespace uia {

// 跨线程查询用的消息（provider → UI 线程）。同一进程、SendMessage 同步，
// 所以 ItemInfo 里的 std::wstring 在返回前一直有效。
constexpr UINT kMsgCount = WM_APP + 0x51;   // 返回标签数
constexpr UINT kMsgItem = WM_APP + 0x52;    // wParam=index, lParam=ItemInfo*
constexpr UINT kMsgSelect = WM_APP + 0x53;  // wParam=index（UIA 选中）

struct ItemInfo {
    std::wstring name;
    std::wstring path;
    RECT rect{};          // 客户区坐标
    bool selected = false;
};

// WM_GETOBJECT 入口：lParam == OBJID_CLIENT 时返回 raw element provider。
LRESULT HandleGetObject(TabStrip* strip, HWND hwnd, WPARAM w, LPARAM l);

// 窗口销毁时释放该 hwnd 的根 provider（UIA 客户端仍持有的引用由 COM 计数管）。
void Release(HWND hwnd);

}  // namespace uia
