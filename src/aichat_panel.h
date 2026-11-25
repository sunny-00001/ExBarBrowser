// AI 面板：右侧一块 WebView2 区域，承载聊天界面。
//
// 【形态】不新建顶层窗口，而是在内容区 viewArea_ 里**占一条矩形**：
//   · 面板自己是一个 STATIC 类子窗口（仅作 WebView2 的宿主与裁剪边界）
//   · WebView2 作为它的子窗口填入整个客户区
//   这样布局只需在 app_layout.cpp 里把 inner.right 减掉面板宽度，
//   与导航窗格（nav_pane）的处理完全同构。
//
// 【为什么不用顶层窗口】顶层窗口要自己管 Z 序、激活、任务栏项、DPI 变化，
//   而且"贴在主窗口右侧"其实做不到（用户拖动主窗口时要跟随）。
//   作为子窗口，这些问题全部由父窗口的 WS_CLIPCHILDREN / WS_CLIPSIBLINGS 和
//   常规布局解决。父窗口是 viewArea_（与导航树、文件视图、三条边界条同父），
//   这样 AI 边界条才能作为兄弟窗口压在面板之上，画出唯一那条中线。
//
// 【收起 = 隐藏而不是销毁】Stop() 会关掉整个浏览器进程，再展开要重新启动
//   WebView2（约 1 秒白屏 + 重新登录态）。收起时只 SetVisible(false) 并把
//   宽度归零，内容与滚动位置都保留。
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aichat_service.h"
#include "aichat_webview.h"
#include "theme.h"

namespace aiui {

// 面板宽度由设置项 aiPanelWidth 决定（物理像素，见 layout_limits.h），
// 拖动文件区右侧边界即可改宽；这里不再保留一份编译期 DIP 常量。
class Panel {
public:
    Panel() = default;
    ~Panel();
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    // 在内容区容器（viewArea_）里创建面板（子窗口 + WebView2）。此时面板收起。
    //   parentHwnd —— 内容区容器
    //   exeDir     —— 用于定位 aichat/ 与 WebView2Loader.dll
    bool Create(HWND parentHwnd, const std::wstring& exeDir, const Theme* theme);

    // 展开/收起。展开时若服务尚未就绪会显示占位页而不是白屏。
    void Show(bool show);
    bool visible() const { return visible_; }
    void Toggle() { Show(!visible_); }

    // 面板占用宽度（像素，已按 DPI 缩放）。收起时为 0。
    int WidthPx() const;

    // 父窗口（viewArea_）布局时调用：rc 是**父窗口客户区**里的目标矩形。
    void SetBounds(const RECT& rc);

    // 主程序主题（App::ApplyTheme）变化后调用：同时给占位页和 WebView2 页面换肤。
    void SyncTheme();

    // 主窗口缩放/移动、DPI 变化后让 WebView2 重新校准（它对新父窗口位置有缓存）。
    void NotifyParentMoved();

    // 供窗口过程转发消息。返回 true = 已处理。
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

    // 页面窗口动作的转发口。App 层决定 minimize / close / maximize / pin 怎么落。
    void SetWindowActionCallback(std::function<void(const std::wstring&)> cb) {
        web_.SetWindowActionCallback(std::move(cb));
    }

    // 页面 AI 状态转发口。宿主用它更新状态栏 chip 与 peek，面板收起也照收。
    void SetStatusCallback(std::function<void(const AiWebStatus&)> cb) {
        web_.SetStatusCallback(std::move(cb));
    }
    // 原生启动进度回调：WebView2 还没上报第一条页面状态时，chip 用它显示
    // "准备中 / 创建中 / 不可用"。页面状态一旦到达，App 侧自动让位。
    void SetNativeActivityCallback(std::function<void()> cb) {
        onNativeActivity_ = std::move(cb);
    }

    // 关闭与清理（主窗口销毁前调）。会终止 node 子进程。
    void Destroy();

    HWND hwnd() const { return host_; }
    const std::wstring& lastError() const { return error_; }

    // ---- 排查面（供 exbar/state 读，见 ai_api.h 的 aiPanelPort 说明）----
    //
    // 面板白屏时，"卡在哪一步"必须能从状态里读出来，不能只存在于日志里。
    // 这三个是只读快照，不改变面板状态。
    unsigned servicePort() const { return servicePort_; }
    // 服务在 InitAiPanel 里就启动了，而 EnsureWebView 要到展开时才跑 ——
    // 端口必须在启动后立刻交进来，否则"面板还没展开"时状态里端口是 0，
    // 那条"用浏览器直接访问"的退路就被一起藏掉了。
    void SetServicePort(unsigned port) {
        if (servicePort_ == port) return;
        servicePort_ = port;
        if (host_) InvalidateRect(host_, nullptr, TRUE);
        NotifyNativeActivity();
    }
    // 浏览器视图三态。用字符串而不是 bool，因为"还没尝试"与"尝试了正在建"
    // 对排查的意义不同：前者说明消息没到，后者说明回调没回来。
    // 异步失败后 error_/showingPlaceholder_ 会把状态改成 failed。
    std::wstring webViewState() const {
        if (web_.Ready()) return L"ready";
        if (showingPlaceholder_ && !error_.empty()) return L"failed";
        return webStartAttempted_ ? L"creating" : L"none";
    }

    // 手动重试（WebView2 创建失败后用户点"重试"）。
    void Retry();

private:
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    void OnHostCreate();
    void OnHostPaint();
    void EnsureWebView();
    void ShutdownWebView();
    void ShowErrorPage(const std::wstring& text);
    void NotifyNativeActivity();
    // WebView2 首次就绪后，用一次宿主的尺寸轻推强制合成器呈现内容（见
    // common.h 的 WM_APP_AI_WEBVIEW_PRESENT 说明）。只做有限次，避免抖动。
    void KickWebViewPresent();

    // 占位页上"重试"按钮的矩形（绘制与点击命中共用这一份几何）。
    // 返回 true = 当前应该显示/命中按钮。
    bool RetryButtonRect(RECT* result) const;

    HWND main_ = nullptr;   // 父窗口（viewArea_）；名字保持历史口径，避免改动调用面
    HWND host_ = nullptr;
    std::wstring error_;
    // 占位页的配色与字体来源。指向 App 的 Theme（生命周期由 App 保证：
    // Theme 是 App 的成员，面板销毁早于它）。
    const Theme* theme_ = nullptr;

    WebView web_;
    bool webStartAttempted_ = false;
    // 宿主 0×0 时延迟创建 WebView2 的重投次数（防止无限重投）。
    int webCreateRetries_ = 8;

    bool visible_ = false;
    unsigned servicePort_ = 0;
    RECT lastBounds_{0, 0, 0, 0};
    bool hasBounds_ = false;

    // 面板里显示的是"服务未就绪"占位页时的标记，便于服务起来后自动刷新。
    bool showingPlaceholder_ = false;
    // 尺寸轻推已完成的标记（每次 WebView2 新建后重置一次）。
    bool presentKicked_ = false;
    std::function<void()> onNativeActivity_;
};

}  // namespace aiui