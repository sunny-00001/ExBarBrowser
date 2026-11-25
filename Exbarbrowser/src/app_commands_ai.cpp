// AI 面板的命令（ai.*）。
//
// 【为什么单独一个文件】命令注册按作用域分文件（app_commands_files / _fs / _view…）。
// AI 面板不是文件操作、也不是视图操作，它管的是一个独立宿主窗口 ——
// 混进 app_commands_misc 会让那个文件同时讲"设置/工具/语言"和"面板生命周期"，
// 正是规则里说的"说不清就是两个文件"。
#include "app.h"

#include "app_commands.h"   // cmd_reg::Register
#include "commands.h"
#include "log.h"

void App::RegisterAiPanelCommands() {
    // 局部名必须叫 reg：tools/check_commands.py 按 "reg(CmdId::" 做静态审计。
    auto reg = cmd_reg::Register;

    reg(CmdId::AiPanelToggle, [this](const Args&) {
        // 面板没建起来（WebView2 运行时缺失、或创建失败）：明确报不可用。
        // 否则调用方会以为"切换成功但看不见"，这是最难查的一类反馈。
        if (!aiPanel_.hwnd()) {
            CommandBus::Get().NoteFailure(L"AI 面板不可用", FailKind::NotSupported);
            return false;
        }
        ToggleAiPanel();
        return true;
    });

    reg(CmdId::AiPanelWeb, [this](const Args&) {
        // 用系统浏览器打开面板服务。
        //
        // 【为什么这条是必需的，不是便利】内嵌 WebView2 依赖运行时版本与
        // 异步回调派发，任一环节出问题就白屏（本机实测：环境创建返回 S_OK，
        // 但完成回调从未送达，浏览器进程根本没起来）。而 node 服务与它是
        // 两件独立的事 —— 服务通常完全正常，加载的还是**同一份** ui/app.js。
        // 这条命令把"服务好的、只是内嵌视图不画"变成一条可用的路。
        const unsigned port = aiPanel_.servicePort();
        if (!port) {
            // 端口为 0 = 服务没起来。这时给浏览器地址也没用（会 ERR_CONNECTION_REFUSED），
            // 所以要如实报原因，而不是打开一个错误页让用户猜。
            CommandBus::Get().NoteFailure(L"AI 服务未启动（面板服务端口为空）",
                                         FailKind::NotSupported);
            LogDbg(L"[aiui] ai.panel.web：服务端口为 0，拒绝打开浏览器");
            return false;
        }

        wchar_t url[64] = L"";
        swprintf(url, 64, L"http://127.0.0.1:%u/", port);
        // ShellExecuteW 的返回值 > 32 才是成功（历史约定：<=32 是错误码）。
        const HINSTANCE r = ShellExecuteW(hwnd_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            CommandBus::Get().NoteFailure(L"打开浏览器失败", FailKind::NotSupported);
            LogDbg(L"[aiui] ai.panel.web：ShellExecute 失败（r=%lld）url=%s",
                   static_cast<long long>(reinterpret_cast<INT_PTR>(r)), url);
            return false;
        }
        LogDbg(L"[aiui] ai.panel.web：已在浏览器打开 %s", url);
        return true;
    });
}