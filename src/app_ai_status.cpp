// 状态栏 AI chip / peek 的 App 侧编排。
//
// 这里不碰 SSE：页面把状态经 WebMessage 送进来，App 只把它落到两个地方 ——
// 状态栏段文本与 peek 气泡。面板是否可见只影响"要不要自动弹 peek"，不影响
// 状态接收本身（收起后面板仍隐藏但存活，SSE 照跑）。
//
// 页面还没加载完时 chip 也不该停在"空闲"：Panel 的原生生命周期会回调
// RefreshAiNativeStatus()，先把"准备中 / 创建中 / 不可用"显示在同一个 chip 上。
// 真实页面状态到达后，原生占位自动让位。
#include "app.h"

#include "aichat_webview_msg.h"
#include "commands.h"
#include "log.h"

namespace {

aiui::AiPeekTone StateTone(const std::wstring& state) {
    if (state == L"busy" || state == L"starting") return aiui::AiPeekTone::Busy;
    if (state == L"approval") return aiui::AiPeekTone::Approval;
    if (state == L"done") return aiui::AiPeekTone::Done;
    if (state == L"error") return aiui::AiPeekTone::Error;
    return aiui::AiPeekTone::Idle;
}

bool NormalizeIncomingState(std::wstring* state) {
    if (!aiui::IsWebViewStatusState(*state)) {
        *state = L"idle";
        return false;
    }
    return true;
}

}  // namespace

void App::ApplyAiStatusCore(const std::wstring& state, const std::wstring& summary,
                            const std::wstring& detail, bool canUndo,
                            const std::wstring& undoDomain, int turnId, bool fromPage) {
    aiStatusState_ = state;
    aiStatusSummary_ = aiui::ClipWebViewStatusText(summary, 160);
    aiStatusDetail_ = aiui::ClipWebViewStatusText(detail, 400);
    // 撤销域必须由宿主侧再夹一次：页面即使被攻破也不能让 peek 去撤一个未知域。
    aiStatusCanUndo_ = canUndo && aiui::IsWebViewUndoDomain(undoDomain)
        && !undoDomain.empty();
    aiStatusUndoDomain_ = aiStatusCanUndo_ ? undoDomain : L"";
    aiStatusTurnId_ = turnId > 0 ? turnId : 0;
    if (fromPage) aiPageStatusSeen_ = true;

    // 未读是"收起期间出现了还没被用户看过的终态结果"。
    // 自动弹 peek 不算已读 —— 用户可能不在电脑前；只有显式消费才清。
    // starting / busy 是过程态，不会点亮；原生 error 与页面 error 走同一档。
    if (state == L"idle") aiUnread_ = false;
    else if (aiui::ShouldRaiseAiUnread(state, aiPanel_.visible())) aiUnread_ = true;
    else if (aiPanel_.visible()) aiUnread_ = false;
    RefreshAiChip();

    // 面板展开时用户已经能直接看到结果，不再用气泡遮住视图。
    if (aiPanel_.visible()) {
        HideAiPeek();
        return;
    }
    if (aiui::IsAiTerminalState(state)) {
        // 自动弹 peek 是可关的（aiPeekEnabled）：有人让 AI 后台跑长任务，
        // 不希望每次终态都盖住正在看的文件列表。关掉后 chip 仍显示未读数与
        // 颜色，通知降级为"看得见但不打断"。
        if (Config::Get().s().aiPeekEnabled) ShowAiPeek();
        else HideAiPeek();
    } else {
        HideAiPeek();
    }
}

void App::ApplyAiWebStatus(const std::wstring& stateRaw, const std::wstring& summary,
                           const std::wstring& detail, bool canUndo,
                           const std::wstring& undoDomain, int turnId) {
    std::wstring state = stateRaw;
    const bool known = NormalizeIncomingState(&state);
    // WebMessage 白名单已校验过，这里再夹一次未知态：非法消息落回空闲，
    // 不能让页面伪造 starting 来重置宿主自己的启动进度。
    ApplyAiStatusCore(state, known ? summary : L"AI · 空闲", detail,
                      canUndo, undoDomain, turnId, true);
}

void App::ApplyAiNativeStatus(const std::wstring& state, const std::wstring& summary,
                              const std::wstring& detail) {
    // 真实结果优先：页面已经报过状态后，原生过程态（idle / starting）不再回写。
    // WebView2 后来失败是例外 —— 那是一条新结果，用户需要看到失败。
    if (aiPageStatusSeen_ && state != L"error") return;
    // WebView2 重建意味着旧页面状态作废，新的 starting 会开启一个观察窗口。
    if (state == L"starting") aiPageStatusSeen_ = false;
    ApplyAiStatusCore(state, summary, detail, false, L"", 0, false);
}

void App::RefreshAiNativeStatus() {
    const aiui::AiNativeActivity native = aiui::ResolveAiNativeActivity(
        aisvc::Enabled(), aiPanel_.webViewState(), aiPanel_.lastError());
    ApplyAiNativeStatus(native.state, native.summary, native.detail);
}

void App::RefreshAiChip() {
    std::wstring tip = aiStatusSummary_;
    if (!aiStatusDetail_.empty()) tip += L"\r\n" + aiStatusDetail_;
    if (aiStatusCanUndo_) tip += L"\r\n点击 chip 可查看“撤销”";
    if (aiUnread_) tip += L"\r\n有未读结果，点击 chip 查看";
    statusBar_.SetSegmentText(Seg_Ai, aiui::AiChipText(aiStatusState_, aiUnread_), tip);
}

void App::ConsumeAiUnread() {
    if (!aiUnread_) return;
    aiUnread_ = false;
    RefreshAiChip();
}

void App::ShowAiPeek() {
    RECT anchor{};
    if (!statusBar_.SegmentScreenRect(Seg_Ai, &anchor)) return;

    aiui::AiPeekView view;
    view.tone = StateTone(aiStatusState_);
    view.title = L"AI · " + std::wstring(aiui::AiStateLabel(aiStatusState_));
    view.summary = aiStatusSummary_.empty() ? view.title : aiStatusSummary_;
    view.detail = aiStatusDetail_;
    view.canUndo = aiStatusCanUndo_;
    view.undoDomain = aiStatusUndoDomain_;
    view.turnId = aiStatusTurnId_;
    aiPeek_.Show(view, anchor);
}

void App::ToggleAiPeek() {
    if (aiPeek_.visible()) HideAiPeek();
    else ShowAiPeek();
    // chip 是唯一入口：点开或收起都表示用户已经在处理这条结果。
    ConsumeAiUnread();
}

void App::HideAiPeek() {
    aiPeek_.Hide();
}
