// 模态消息泵实现（说明见 modal.h）。
#include "modal.h"

namespace modal {
namespace {

// 当前嵌套的模态层数。文件内唯一一份 —— 维护两份计数必然会漏。
int g_depth = 0;

}  // namespace

bool Run(const std::function<bool()>& done, const std::function<bool(MSG&)>& onKey) {
    Scope guard;   // 计数 +1/-1：异常/早退路径也不会漏减
    MSG msg{};
    int gm = 1;    // 初值必须非 0：首拍 done() 为真时循环体不执行，
                   // 未初始化就读 gm 是 UB（当前调用点都不会命中，但这是复制就中招的模式）。
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

int Depth() {
    return g_depth;
}

Scope::Scope() {
    ++g_depth;
}

Scope::~Scope() {
    if (g_depth > 0) --g_depth;
}

}  // namespace modal
