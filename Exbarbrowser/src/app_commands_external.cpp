// 带参外部入口：clipboard.copy / ext.terminal.at。
//
// 与 misc 分片的区别是作用域：这里的命令**只由参数点名目标**，
// 不读当前选中、不读当前目录。人侧从书签/面包屑右键把路径塞进参数，
// 机器侧直接给路径；两条入口共用同一个 handler。
#include "app.h"

#include "app_commands.h"
#include "clipboard.h"

void App::RegisterExternalCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    regR(CmdId::ClipboardCopy, [this](const Args& args, json::Value* out) -> bool {
        const std::wstring text = args.Str(L"text");
        if (!SetClipboardText(hwnd_, text)) {
            CommandBus::Get().NoteFailure(L"写入剪贴板失败（被其它程序占用？）");
            return false;
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"text", json::Value::MakeStr(text));
            *out = std::move(root);
        }
        return true;
    });

    reg(CmdId::ExtTerminalAt, [this](const Args& args) {
        const std::wstring path = args.Str(L"path");
        if (path.empty()) {
            CommandBus::Get().NoteFailure(L"缺少必填参数：path");
            return false;
        }
        RunTerminal(hwnd_, path, false);
        return true;
    });
}
