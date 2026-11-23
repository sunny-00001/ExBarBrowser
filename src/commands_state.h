#pragma once
#include <functional>
#include <string>

struct CommandDef;

// ---------------------------------------------------------------- 可否执行
//
// 判定一个命令此刻能不能执行，并给出**人可读原因**。
//
// 状态从哪来：App 在启动时注册一个 StateProbe，commands 层不去反向依赖 App。
// 这样判定逻辑可独立单测（喂假状态即可），也不会把窗口句柄带进命令表。
struct CommandState {
    int historyBack = 0;      // 可后退的条数
    int historyForward = 0;   // 可前进的条数
    bool hasSelection = false;
    int selectionCount = 0;   // 选中项个数（HasSelectionSingle 用）
    int tabCount = 0;
    int activeTabIndex = 0;   // 当前标签的位置（判断能不能左右移）
    bool hasClipboard = false;
    bool canGoUp = false;
    bool hasClosedTabs = false;   // 有可重新打开的已关闭标签
};

using StateProbe = std::function<CommandState()>;

// 注册状态探测（App 启动时调一次）。未注册时视为"一切前置都不满足"——
// 失败关闭，宁可不执行也不误判为可执行。
void SetStateProbe(StateProbe fn);

// 返回 true = 可执行。false 时若 reason 非空则填入人可读原因。
// 这是 UI 灰按钮与机器调用前检查的**唯一**入口。
bool CanExecute(const CommandDef& def, std::wstring* reason = nullptr);
