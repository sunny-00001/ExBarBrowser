// 状态栏 AI chip 的 peek 气泡与 chip 文案口径。
//
// 【为什么不是 tooltip】tooltip 只能显示文字，装不下"打开对话 / 撤销"两个动作；
// 面板收起后，用户需要在不展开面板的情况下决定要不要看结果、要不要回滚。
// 【为什么是独立顶层小窗】它锚在状态栏 chip 上方，但不进 tab 顺序、不激活、
// 不出现在任务栏；点击 chip 再开一次会收起它。
#pragma once
#include <windows.h>

#include <functional>
#include <string>

#include "theme.h"

namespace aiui {

// 状态的显示名与"终态"判定只有一份口径：chip、peek 标题、未读判定共用。
// 终态 = 用户还没看就可能错过的结果（完成 / 失败 / 待确认）；
// busy / starting / idle 是过程态，不产生未读。
const wchar_t* AiStateLabel(const std::wstring& state);
bool IsAiTerminalState(const std::wstring& state);
bool ShouldRaiseAiUnread(const std::wstring& state, bool panelVisible);
// chip 上的未读标记用同一枚圆点，避免各写一份造成"有未读但看不出来"。
std::wstring AiChipText(const std::wstring& state, bool unread);

// 页面还没上报状态前，状态栏显示宿主自己知道的启动进度。
// 这只是一个占位活动态：WebMessage 白名单不接受 starting，页面不能伪造它；
// 原生态也不能覆盖已经到达的真实页面结果。
struct AiNativeActivity {
    std::wstring state;     // idle / starting / error
    std::wstring summary;
    std::wstring detail;
};
AiNativeActivity ResolveAiNativeActivity(bool aiEnabled,
                                         const std::wstring& webViewState,
                                         const std::wstring& error);

enum class AiPeekTone {
    Idle = 0,
    Busy,
    Approval,
    Done,
    Error
};

struct AiPeekView {
    AiPeekTone tone = AiPeekTone::Idle;
    std::wstring title;     // "AI · 完成" 等短标题
    std::wstring summary;   // 一行完成摘要
    std::wstring detail;    // 可选补充
    bool canUndo = false;
    std::wstring undoDomain;
    int turnId = 0;
};

// 纯几何：气泡贴着状态栏 chip 的右上角，优先向上展开，越屏则夹回工作区。
// 测试直接验这个函数，不发合成鼠标、不碰屏幕。
struct AiPeekLayout {
    RECT window{};
    RECT openButton{};
    RECT undoButton{};
    RECT closeButton{};
};
AiPeekLayout ComputeAiPeekLayout(const RECT& anchorScreen, int width, int height,
                                 const RECT& workArea);

class StatusPeek {
public:
    StatusPeek() = default;
    ~StatusPeek() { Destroy(); }
    StatusPeek(const StatusPeek&) = delete;
    StatusPeek& operator=(const StatusPeek&) = delete;

    void Create(HWND owner, const Theme* theme);
    void Destroy();
    void SyncTheme();

    void Show(const AiPeekView& view, const RECT& anchorScreen);
    void Hide();
    bool visible() const { return hwnd_ && IsWindow(hwnd_) && IsWindowVisible(hwnd_); }

    void SetOpenFn(std::function<void()> fn) { onOpen_ = std::move(fn); }
    void SetUndoFn(std::function<void(const std::wstring&)> fn) { onUndo_ = std::move(fn); }
    // 关闭按钮与"打开/撤销"不同：它只表示"这条结果我看过了"，不触发别的动作。
    void SetDismissFn(std::function<void()> fn) { onDismiss_ = std::move(fn); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);
    void RebuildRects();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    const Theme* theme_ = nullptr;
    AiPeekView view_;
    AiPeekLayout rects_;
    std::function<void()> onOpen_;
    std::function<void(const std::wstring&)> onUndo_;
    std::function<void()> onDismiss_;
};

}  // namespace aiui
