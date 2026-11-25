// 破坏性命令的人工确认：App 侧的弹框实现。
//
// 【为什么独立成文件】app.cpp 已超 500 行（欠账棘轮记在
// tests/baselines/file-length-baseline.json），而这一段确认框只有十来行 ——
// 塞在 app.cpp 里只会让那个文件继续涨。
//
// 【这一层负责什么】
// CommandBus 不持有窗口引用（与 CanExecute 同一口径：CommandDef 是纯数据，
// 判定由调用方做），所以"问用户"这件事必须由 App 提供实现。
// 全程只有这一处弹框 —— 命令面板、序列、弹层菜单、快捷键、鼠标都经
// CommandBus::Invoke，在这里统一过一道门。
//
// 【AI 侧不走这里】危险双门（aiAllowDanger + approve）在 ai_api.cpp 管，
// 机器调用不会卡在模态框里（弹了就把管道调用卡到超时）。
#include "app.h"

#include "commands.h"

void App::InstallConfirmCallback() {
    CommandBus::Get().SetConfirmFn([this](const CommandDef& d) {
        const int yes = MessageBoxW(
            hwnd_,
            (L"「" + std::wstring(d.title) + L"」会覆盖现有内容，且可能无法撤销。确定执行吗？")
                .c_str(),
            L"确认执行", MB_YESNO | MB_ICONWARNING);
        return yes == IDYES;
    });
}
