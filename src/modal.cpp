// 模态消息泵实现（说明见 modal.h）。
#include "modal.h"

namespace modal {

bool Run(const std::function<bool()>& done, const std::function<bool(MSG&)>& onKey) {
    MSG msg{};
    int gm;
    // GetMessageW：>0 有消息，0 = WM_QUIT，-1 = 错误。三者必须分开处理。
    while (!done() && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (onKey && onKey(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) {
        // WM_QUIT 是宿主的意图，不能吞掉：原样交还，否则进程退不出去。
        PostQuitMessage(static_cast<int>(msg.wParam));
        return false;
    }
    return true;
}

}  // namespace modal
