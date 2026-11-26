// 文件列表区的上下文菜单命令（Shift+F10）。
//
// 单独成文件而不是塞进 app_commands_files.cpp：那个文件已经 1000+ 行，
// 而"弹 shell 菜单"这条链路（IContextMenu → owner-draw 转发 → 菜单跟踪）
// 与命令注册不是一回事，混在一起会让真正需要一口气读完的部分被淹没。
//
// 机制见 src/shell_ops_ctxmenu.cpp。
#include "app.h"

#include "app_commands.h"
#include "shell_ops.h"

void App::RegisterContextCommands() {
    auto reg = cmd_reg::Register;
    reg(CmdId::FileContextMenu, [this](const Args&) {
        return WithView([this](IShellView* v) {
            // 锚点：光标位置。键盘触发（Shift+F10）时用光标当前位置 ——
            // Explorer 的菜单键也是这么弹的（菜单出现在光标旁，不是屏幕中央）。
            POINT at{};
            if (!GetCursorPos(&at)) {
                RECT wr{};
                GetWindowRect(hwnd_, &wr);
                at = POINT{wr.left + 24, wr.top + 24};
            }
            // 菜单跟踪完要把焦点还给视图 —— 菜单关掉后焦点停在 owner 窗口上，
            // 方向键/直接输入就都失效了（与 nav.focus 的处理同口径）。
            const bool ok = shellops::ShowContextMenu(v, hwnd_, at);
            FocusActiveView();
            return ok;
        }, L"上下文菜单");
    });
}
