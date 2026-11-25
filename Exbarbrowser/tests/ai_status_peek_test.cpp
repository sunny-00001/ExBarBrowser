// peek 气泡的纯几何回归：不创建窗口、不发鼠标，只验证锚点与按钮布局。
#include <cstdio>

#include "ai_status_peek.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool Inside(const RECT& inner, const RECT& outer) {
    return inner.left >= outer.left && inner.right <= outer.right
        && inner.top >= outer.top && inner.bottom <= outer.bottom;
}

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

}  // namespace

int main() {
    using aiui::ComputeAiPeekLayout;
    const RECT work{0, 0, 1920, 1040};

    std::printf("常规锚点\n");
    {
        const RECT anchor{900, 760, 1000, 780};
        const auto l = ComputeAiPeekLayout(anchor, 360, 156, work);
        Check(l.window.right <= anchor.right, "气泡右缘不越过 chip 右缘");
        Check(l.window.bottom <= anchor.top, "空间够时气泡在 chip 上方");
        Check(Inside(l.openButton, l.window), "打开按钮在气泡内");
        Check(Inside(l.undoButton, l.window), "撤销按钮在气泡内");
        Check(Inside(l.closeButton, l.window), "关闭按钮在气泡内");
        Check(!Overlaps(l.openButton, l.undoButton), "打开与撤销不重叠");
        Check(!Overlaps(l.openButton, l.closeButton), "打开与关闭不重叠");
    }

    std::printf("\n贴边锚点\n");
    {
        const RECT anchor{900, 20, 1000, 40};
        const auto l = ComputeAiPeekLayout(anchor, 360, 156, work);
        Check(Inside(l.window, work), "上方空间不足时气泡仍在屏幕工作区内");
        Check(l.window.top >= anchor.bottom, "翻到 chip 下方而不是被裁掉");
    }
    {
        const RECT anchor{1880, 760, 2500, 780};
        const auto l = ComputeAiPeekLayout(anchor, 360, 156, work);
        Check(Inside(l.window, work), "右侧越界时夹回工作区");
        Check(l.window.right <= work.right, "右缘不超过工作区右界");
    }

    std::printf("\n未读状态口径\n");
    {
        using aiui::AiChipText;
        using aiui::AiStateLabel;
        using aiui::IsAiTerminalState;
        using aiui::ShouldRaiseAiUnread;
        Check(std::wstring(AiStateLabel(L"busy")) == L"运行中", "busy 显示运行中");
        Check(std::wstring(AiStateLabel(L"starting")) == L"准备中", "starting 显示准备中");
        Check(std::wstring(AiStateLabel(L"approval")) == L"待确认", "approval 显示待确认");
        Check(std::wstring(AiStateLabel(L"done")) == L"完成", "done 显示完成");
        Check(std::wstring(AiStateLabel(L"error")) == L"失败", "error 显示失败");
        Check(std::wstring(AiStateLabel(L"nope")) == L"空闲", "未知状态退回空闲");

        Check(IsAiTerminalState(L"done"), "done 是终态");
        Check(IsAiTerminalState(L"error"), "error 是终态");
        Check(IsAiTerminalState(L"approval"), "approval 是终态");
        Check(!IsAiTerminalState(L"busy"), "busy 不是终态");
        Check(!IsAiTerminalState(L"starting"), "starting 不是终态");
        Check(!IsAiTerminalState(L"idle"), "idle 不是终态");

        Check(ShouldRaiseAiUnread(L"done", false), "收起时的完成结果点亮未读");
        Check(ShouldRaiseAiUnread(L"error", false), "收起时的失败结果点亮未读");
        Check(ShouldRaiseAiUnread(L"approval", false), "收起时的待确认点亮未读");
        Check(!ShouldRaiseAiUnread(L"done", true), "面板展开时不制造未读");
        Check(!ShouldRaiseAiUnread(L"busy", false), "过程态不制造未读");
        Check(!ShouldRaiseAiUnread(L"starting", false), "原生启动态不制造未读");

        const std::wstring plain = AiChipText(L"done", false);
        const std::wstring unread = AiChipText(L"done", true);
        Check(plain == L"AI · 完成", "chip 文案沿用状态口径");
        Check(unread == plain + L" ●", "未读 chip 追加可见圆点");
        Check(AiChipText(L"idle", true) == L"AI · 空闲 ●", "空闲态也保留未读提示");
        Check(AiChipText(L"starting", false) == L"AI · 准备中", "启动态沿用状态口径");
    }

    std::printf("\n原生启动态解析\n");
    {
        using aiui::ResolveAiNativeActivity;
        const auto off = ResolveAiNativeActivity(false, L"none", L"");
        Check(off.state == L"idle", "AI 未启用时 chip 回空闲");
        Check(off.detail.find(L"设置") != std::wstring::npos, "未启用给出可操作提示");

        const auto none = ResolveAiNativeActivity(true, L"none", L"");
        Check(none.state == L"starting", "已启用但还没创建视图时显示准备中");

        const auto creating = ResolveAiNativeActivity(true, L"creating", L"");
        Check(creating.state == L"starting" && creating.summary.find(L"创建") != std::wstring::npos,
              "创建中给出明确进度");

        const auto ready = ResolveAiNativeActivity(true, L"ready", L"");
        Check(ready.state == L"idle", "WebView2 就绪后回空闲等页面状态");

        const auto failed = ResolveAiNativeActivity(true, L"failed", L"WebView2 缺失");
        Check(failed.state == L"error" && failed.detail == L"WebView2 缺失", "失败原因带入详情");
    }

    std::printf("\n结果: %d 失败\n", g_fail);
    return g_fail;
}
