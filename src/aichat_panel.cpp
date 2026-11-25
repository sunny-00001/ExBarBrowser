// AI 面板实现。形态与生命周期说明见 aichat_panel.h。
#include "aichat_panel.h"

#include "config.h"     // 面板宽度是这个配置字段唯一的落点
#include "identity.h"   // 窗口类名唯一来源（规则 B1）
#include "log.h"
#include "path_util.h"
#include "text_file.h"   // tf::Join（路径拼接，处理尾分隔符差异）
#include "ui_draw.h"     // FillRectColor / DrawTextRow（唯一绘制实现）

#include <cwchar>

namespace aiui {
namespace {

bool g_classRegistered = false;

LRESULT CALLBACK PanelHostThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    LRESULT out = 0;
    if (self->HandleMessage(m, w, l, &out)) return out;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

Panel::~Panel() { Destroy(); }

bool Panel::Create(HWND parentHwnd, const std::wstring& exeDir, const Theme* theme) {
    if (host_) return true;
    main_ = parentHwnd;
    // exeDir 参数保留是为了不动 app.cpp 的调用契约；WebView2 用户目录现在走
    // Config::Dir()（profile 感知），不再从这里派生，故不再存成成员。
    (void)exeDir;
    theme_ = theme;
    web_.SetTheme(theme_ ? theme_->dark : true);

    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PanelHostThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = id::kAiPanelHostClass;
        // 【hbrBackground 必须显式为 nullptr，而且光靠它不够】
        //
        // 这里原本只有一句注释"不设背景刷"，代码里也就没赋值（零初始化即
        // nullptr）。但 nullptr **不等于**"不擦背景"：WM_ERASEBKGND 一旦
        // 交回 DefWindowProc，它会用系统默认的 COLOR_WINDOW 白刷
        // （255,255,255）铺满客户区 —— 于是自绘的占位页被整块盖掉，
        // 面板看起来是"空着的"。实测截图右侧正是纯白 255,255,255，
        // 而不是我们画的底色。
        // 两道保险：这里显式写 nullptr（表明意图），并且窗口过程对
        // WM_ERASEBKGND / WM_PAINT 一律自己处理、不交回默认实现。
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error_ = L"注册面板窗口类失败";
            LogDbg(L"[aiui] %s (err=%lu)", error_.c_str(), GetLastError());
            return false;
        }
        g_classRegistered = true;
    }

    host_ = CreateWindowExW(
        0, id::kAiPanelHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host_) {
        error_ = L"创建面板窗口失败";
        LogDbg(L"[aiui] %s (err=%lu)", error_.c_str(), GetLastError());
        return false;
    }
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // WebView2 常在宿主还是 0×0 时发起创建（面板展开那一帧 Layout 未必先跑）。
    // 控制器就绪后回调一次，用宿主**当前**客户区补一次真实 bounds + 可见 ——
    // 这是"进程齐全、导航成功却一片空白"的根因修复（见 docs/DECISIONS.md D14）。
    web_.SetReadyCallback([this]() {
        if (!host_) return;
        RECT rc{};
        if (GetClientRect(host_, &rc) && rc.right > 0 && rc.bottom > 0) {
            web_.SetBounds(rc);
        }
        web_.SetVisible(visible_);
        LogDbg(L"[aiui] 就绪回调：重设宿主客户区 %ldx%ld visible=%d",
               static_cast<long>(rc.right), static_cast<long>(rc.bottom),
               visible_ ? 1 : 0);
        // 【强制呈现】首次创建时 WebView2 常在宿主已是最终尺寸的情况下就绪，
        // put_Bounds 给的是相同矩形，合成器不建立可见表面 → 面板纯白。投一条
        // 消息，等循环空闲后做一次真实尺寸变化（缩小 2px 再还原），逼它呈现。
        presentKicked_ = false;
        if (host_ && visible_) {
            PostMessageW(host_, WM_APP_AI_WEBVIEW_PRESENT, 0, 0);
        }
        NotifyNativeActivity();
    });

    web_.SetErrorCallback([this](const std::wstring& reason) {
        error_ = reason;
        showingPlaceholder_ = true;
        webStartAttempted_ = false;   // 失败是终态；Retry 要允许重建
        if (host_) InvalidateRect(host_, nullptr, TRUE);
        LogDbg(L"[aiui] WebView2 异步失败：%s", reason.c_str());
        NotifyNativeActivity();
    });

    // 初始收起：宽 0、隐藏。
    visible_ = false;
    ShowWindow(host_, SW_HIDE);
    return true;
}

void Panel::Show(bool show) {
    if (!host_) return;
    if (visible_ == show) return;
    visible_ = show;

    if (show) {
        ShowWindow(host_, SW_SHOW);
        webCreateRetries_ = 8;   // 每次展开重置延迟创建的重投预算
        // 【不在这里直接创建 WebView2】见 common.h 的 WM_APP_AI_PANEL_CREATE。
        // 这一帧消息循环还在命令处理器里，WebView2 的异步回调送不到；
        // 投一条消息，等循环空闲再建。
        if (!web_.Ready() && !webStartAttempted_) {
            PostMessageW(host_, WM_APP_AI_PANEL_CREATE, 0, 0);
        } else if (web_.Ready()) {
            // 从收起恢复：重新把宿主真实客户区推给 WebView2，确保重建可见
            // 合成表面（0×0 → 非 0 时它不会自动恢复，必须显式 put_Bounds）。
            RECT rc{};
            if (GetClientRect(host_, &rc) && rc.right > 0 && rc.bottom > 0) {
                web_.SetBounds(rc);
            }
            web_.SetVisible(true);
            // 同样要在循环空闲后轻推一次：0×0 → 非 0 的恢复也可能停在
            // "有内容但没呈现"的纯白态（与首次创建同源，见 common.h 说明）。
            presentKicked_ = false;
            PostMessageW(host_, WM_APP_AI_WEBVIEW_PRESENT, 0, 0);
        }
    } else {
        // 收起只是隐藏 —— 见头文件说明（销毁要重建浏览器进程，代价 1 秒白屏）。
        if (web_.Ready()) web_.SetVisible(false);
        ShowWindow(host_, SW_HIDE);
    }
    // chip 的启动态跟宿主生命周期走：展开后还没建好 WebView2 时显示"准备中"，
    // 收起时也不必把它擦回空闲 —— 页面状态到达时自然会接管。
    NotifyNativeActivity();
    // 让父窗口重新排一次：宽度变化改变了文件区可用宽度。
    // 真正重排由 App::ToggleAiPanel() 的 Layout() 完成；这里只把父窗口标脏。
    if (main_) InvalidateRect(main_, nullptr, FALSE);
}

int Panel::WidthPx() const {
    if (!visible_) return 0;
    // 宽度由 AI 面板边界条拖动写入 aiPanelWidth（物理像素，见 layout_limits.h）。
    // 这里不能再按 DPI 乘一遍：拖拽给的是客户区像素，落盘也是像素，
    // 中间插一次缩放就会让"拖到哪存到哪、重启后又变一截"。
    return Config::Get().s().aiPanelWidth;
}

void Panel::SetBounds(const RECT& rc) {
    if (!host_) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    // 尺寸必须为正。传进负数宽度时 SetWindowPos 会成功但窗口变成"反向矩形"，
    // GetClientRect 随之返回怪值（实测见过 525x1397，比主窗口还高）——
    // 那种情况下自绘会画到窗口外，用户看到的仍是一片空白，却以为是"没画"。
    if (w < 0 || h < 0) {
        LogDbg(L"[aiui] SetBounds 收到负尺寸 %dx%d（l=%d t=%d r=%d b=%d），已忽略",
               w, h, static_cast<int>(rc.left), static_cast<int>(rc.top),
               static_cast<int>(rc.right), static_cast<int>(rc.bottom));
        return;
    }
    if (hasBounds_ && EqualRect(&lastBounds_, &rc)) return;   // 几何没变就别动
    lastBounds_ = rc;
    hasBounds_ = true;

    LogDbg(L"[aiui] SetBounds %dx%d @ (%d,%d)",
           w, h, static_cast<int>(rc.left), static_cast<int>(rc.top));

    SetWindowPos(host_, nullptr, rc.left, rc.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (web_.Ready()) {
        // WebView2 的 Bounds 是**相对父窗口客户区**的。
        RECT inner{0, 0, w, h};
        web_.SetBounds(inner);
    }
}

void Panel::SyncTheme() {
    if (!theme_) return;
    web_.SetTheme(theme_->dark);
    // 占位页是自绘的，也要跟着换底色；否则 WebView2 还没起来时会出现
    // "主程序已浅色、面板仍深色"的半套主题。
    if (host_ && !web_.Ready()) InvalidateRect(host_, nullptr, TRUE);
    LogDbg(L"[aiui] 主题同步 dark=%d", theme_->dark ? 1 : 0);
}

void Panel::NotifyParentMoved() {
    // WebView2 会缓存父窗口位置用于 tooltip/无障碍定位；主窗口移动后要通知它。
    web_.NotifyParentMoved();
    if (web_.Ready() && hasBounds_) {
        RECT inner{0, 0, lastBounds_.right - lastBounds_.left,
                   lastBounds_.bottom - lastBounds_.top};
        web_.SetBounds(inner);
    }
}

void Panel::EnsureWebView() {
    if (web_.Ready() || webStartAttempted_) {
        LogDbg(L"[aiui] EnsureWebView 跳过（ready=%d attempted=%d）",
               web_.Ready() ? 1 : 0, webStartAttempted_ ? 1 : 0);
        return;
    }
    if (!host_) return;

    // 服务未就绪时先别创建 WebView2：不然会先白屏、再跳到错误页，
    // 白屏那一帧很难看。占位页走自绘（见 OnHostPaint）。
    aisvc::Status st = aisvc::Current();
    // 子进程曾起来但现在死了：直接重启，并换用新端口/新 HTTP token。
    // 不这么做的话 UI 会继续导航到死端口，用户只能重启整个程序。
    if (!st.running && !st.reason.empty()) {
        unsigned restartedPort = 0;
        std::wstring restartReason;
        if (aisvc::Restart(&restartedPort, &restartReason)) {
            st = aisvc::Current();
        } else {
            error_ = restartReason;
            showingPlaceholder_ = true;
            InvalidateRect(host_, nullptr, TRUE);
            LogDbg(L"[aiui] AI 服务重启失败：%s", error_.c_str());
            NotifyNativeActivity();
            return;
        }
    }
    if (!st.running) {
        error_ = st.reason.empty() ? L"AI 服务未启动" : st.reason;
        showingPlaceholder_ = true;
        InvalidateRect(host_, nullptr, TRUE);
        LogDbg(L"[aiui] 服务未启用：%s", error_.c_str());
        NotifyNativeActivity();
        return;
    }

    servicePort_ = st.port;
    std::wstring reason;
    if (!RuntimeAvailable(nullptr, &reason)) {
        error_ = reason;
        showingPlaceholder_ = true;
        InvalidateRect(host_, nullptr, TRUE);
        LogDbg(L"[aiui] %s", reason.c_str());
        NotifyNativeActivity();
        return;
    }

    // 【不要在这里等端口可连】WebView2 的环境/控制器创建都是**异步回调**，
// 回调靠本线程的消息循环派发。以前这里用 WaitReady 阻塞 8 秒等 TCP 端口，
// 那段时间消息循环停转 —— 于是即便 g_createEnv 立刻返回成功，回调也永远
// 送不到（表现为"调了 Start() 之后一片安静，没有任何日志"）。
// 服务在 InitAiPanel 里已经启动过；就算它还没监听，WebView2 也会在几秒后
// 自己重试，比卡死 UI 线程好得多。
    LogDbg(L"[aiui] 准备创建 WebView2：port=%u running=%d", st.port, st.running ? 1 : 0);

    // 首帧主题走 URL 参数：WebMessage 可能比页面里的监听器更早送达，
    // 那时运行时切换会丢；URL 参数由 index.html 在 CSS 加载前解析，绝不闪错色。
    // 运行中换肤由 Panel::SyncTheme() 走 WebMessage，不重载页面、聊天上下文不丢。
    std::wstring url =
        L"http://127.0.0.1:" + std::to_wstring(servicePort_) +
        L"/?exbarTheme=" + ((theme_ && theme_->dark) ? L"dark" : L"light");
    const std::wstring httpToken = aisvc::HttpToken();
    if (!httpToken.empty()) url += L"&exbarHttpToken=" + httpToken;

    // 用户数据目录挂在 Config::CacheDir() 下（见 config.h 的六类分层）。
    // 【为什么不是 exeDir 硬编码】CacheDir() 从 profile 根派生，是"当前 profile
    // 的 WebView2 用户数据"唯一来源：不同 profile/实例各有一份，不会共用
    // Chromium 的单例锁，localStorage（会话、确认模式、收藏模型）也不互相污染。
    // WebView2 是纯缓存（可整体删、重建无损失），所以挂在 cache\ 而非 data\。
    // 用 tf::Join：目录尾分隔符不保证存在（见 App::InitAiPanel 的说明）。
    const std::wstring userData = tf::Join(Config::Get().CacheDir(), L"webview2");

    // 【必须在宿主已有真实尺寸时创建控制器】WebView2 在 0×0 父窗口上创建时，
    // 导航会成功、进程齐全，但不建立可见合成表面 —— 表现为一片空白（实测）。
    // 面板展开时 Layout 会把真实矩形 SetBounds 进来；若此刻还没有，重投一次
    // 消息等下一帧（有上限，避免死循环）。
    if (!hasBounds_ || lastBounds_.right - lastBounds_.left <= 0 ||
        lastBounds_.bottom - lastBounds_.top <= 0) {
        LogDbg(L"[aiui] EnsureWebView 宿主尚无尺寸，延迟创建");
        --webCreateRetries_;
        if (webCreateRetries_ > 0 && host_) {
            PostMessageW(host_, WM_APP_AI_PANEL_CREATE, 0, 0);
        }
        NotifyNativeActivity();
        return;
    }

    webStartAttempted_ = true;
    NotifyNativeActivity();   // 状态从 none 切到 creating
    if (!web_.Start(host_, url, userData)) {
        error_ = web_.Reason();
        showingPlaceholder_ = true;
        InvalidateRect(host_, nullptr, TRUE);
        LogDbg(L"[aiui] WebView2 启动失败：%s", error_.c_str());
        NotifyNativeActivity();
        return;
    }
    showingPlaceholder_ = false;
    error_.clear();

    if (hasBounds_) {
        RECT inner{0, 0, lastBounds_.right - lastBounds_.left,
                   lastBounds_.bottom - lastBounds_.top};
        web_.SetBounds(inner);
    }
    web_.SetVisible(visible_);
}

void Panel::Retry() {
    ShutdownWebView();
    webStartAttempted_ = false;
    error_.clear();
    showingPlaceholder_ = false;
    presentKicked_ = false;
    EnsureWebView();
    // WebView2 没起来时仍是占位页，重画一遍让状态（"创建中"/原因）立刻更新。
    // 不重画的话用户点了按钮看不到任何变化，会以为按钮没生效。
    if (!web_.Ready() && host_) InvalidateRect(host_, nullptr, TRUE);
    NotifyNativeActivity();
}

void Panel::NotifyNativeActivity() {
    if (onNativeActivity_) onNativeActivity_();
}

void Panel::ShutdownWebView() {
    web_.Stop();
}

// 占位页上"重试"按钮的矩形。
//
// 【绘制与命中必须共用同一份几何】否则会出现"按钮画在这里、点那里才有反应"
// —— 肉眼完全看不出差别的位移，用户只会觉得按钮坏了。所以两边都调这个函数。
// 返回到 result，返回 true = 当前应该有按钮。
bool Panel::RetryButtonRect(RECT* result) const {
    if (!host_) return false;
    // 只在"确实失败/卡住"时给按钮：页面正在加载时给重试会让用户误点，
    // 把本来会自己完成的加载又打断一次。
    if (error_.empty()) return false;   // 创建中不给按钮，避免打断正常异步创建
    if (web_.Ready()) return false;

    RECT rc{};
    GetClientRect(host_, &rc);
    if (rc.right - rc.left < 120 || rc.bottom - rc.top < 80) return false;

    const int w = 96;
    const int h = 28;
    result->left = rc.left + 20;
    result->top = rc.bottom - 84;      // 在诊断行之上
    result->right = result->left + w;
    result->bottom = result->top + h;
    return true;
}

bool Panel::HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (msg) {
        case WM_APP_AI_PANEL_CREATE:
            // 循环已经空闲：现在建 WebView2，异步回调能送到。
            EnsureWebView();
            return true;
        case WM_APP_AI_WEBVIEW_PRESENT:
            // 就绪后循环空闲：做一次尺寸轻推，逼 WebView2 合成器呈现内容。
            KickWebViewPresent();
            return true;
        case WM_PAINT:
            // 【必须自己完整处理并返回 true】以前这里是 `OnHostPaint(); return false;`
            // —— 我们已经在 OnHostPaint 里 BeginPaint/EndPaint，又把消息交回
            // DefWindowProc，它会再 BeginPaint 一次并用自己的背景刷重画。
            // 窗口类没设 hbrBackground（见 Create），系统就用默认白刷
            // （COLOR_WINDOW = 255,255,255）铺满 —— 于是占位页的内容被整块盖掉，
            // 面板看起来"空着"。实测截图右侧正是纯白 255,255,255，
            // 而不是 panelBg 该有的 252,251,251。
            if (!web_.Ready()) {
                OnHostPaint();
                *result = 0;
                return true;
            }
            return false;   // WebView2 就绪时让它自己画（子窗口覆盖整个客户区）
        case WM_ERASEBKGND:
            // 【一律自己处理】不交给 DefWindowProc：
            //   · WebView2 就绪时它自己填满，我们不该擦；
            //   · 未就绪时由 OnHostPaint 铺背景，也不需要系统先擦一遍。
            // 返回 true（"已擦除"）能阻止系统用默认白刷覆盖我们的绘制。
            *result = 1;
            return true;
        case WM_SETCURSOR:
            // 鼠标悬在重试按钮上时要变成手型，否则用户不知道那里能点。
            // 只改光标形状，不抢焦点（R21 允许：这不是合成输入）。
            {
                RECT hit{};
                if (RetryButtonRect(&hit)) {
                    POINT pt{};
                    if (GetCursorPos(&pt) && ScreenToClient(host_, &pt)
                        && PtInRect(&hit, pt)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        *result = TRUE;
                        return true;
                    }
                }
            }
            return false;
        case WM_LBUTTONDOWN:
            // 点空白处不该有任何作用；只有命中按钮才重试。
            {
                RECT hit{};
                if (RetryButtonRect(&hit)) {
                    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    if (PtInRect(&hit, pt)) {
                        LogDbg(L"[aiui] 用户点了重试");
                        Retry();
                        *result = 0;
                        return true;
                    }
                }
            }
            return false;
        case WM_SIZE:
            if (hasBounds_ && web_.Ready()) {
                RECT inner{0, 0, LOWORD(lp), HIWORD(lp)};
                web_.SetBounds(inner);
            }
            return false;
        default:
            return false;
    }
}

void Panel::KickWebViewPresent() {
    // 一次性的尺寸轻推：把宿主缩小 2px 再还原，制造真实的 width/height 变化。
    // WebView2 的子窗口跟随宿主 Resize，合成器借此建立可见表面、把内部已渲染
    // 的内容真正呈现出来。实测这是"导航成功却一片纯白"的唯一可靠解药。
    if (!host_ || !IsWindow(host_) || !web_.Ready() || !visible_) return;
    if (presentKicked_) return;

    RECT rc{};
    if (!GetWindowRect(host_, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 8 || h <= 8) return;   // 收起态没有可推的尺寸

    presentKicked_ = true;
    const UINT flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    // 缩小 → 还原。这两次 SetWindowPos 会各发一次 WM_SIZE，进而 web_.SetBounds。
    SetWindowPos(host_, nullptr, 0, 0, w - 2, h - 2, flags);
    SetWindowPos(host_, nullptr, 0, 0, w,     h,     flags);
    // 还原后显式把最终客户区再推一次，确保 WebView2 bounds 与宿主一致。
    RECT cr{};
    if (GetClientRect(host_, &cr) && cr.right > 0 && cr.bottom > 0) {
        web_.SetBounds(cr);
    }
    LogDbg(L"[aiui] 尺寸轻推强制呈现：%dx%d → %dx%d → %dx%d",
           w, h, w - 2, h - 2, w, h);
}

void Panel::OnHostPaint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(host_, &ps);
    if (!dc) return;

    RECT rc{};
    GetClientRect(host_, &rc);
    // 画了就留一行日志：占位页"看起来空白"时，第一个要回答的问题就是
    // "它到底被调过没有"。尺寸为 0（面板收起）时不打，避免刷屏。
    if (rc.right > 0 && rc.bottom > 0) {
        LogDbg(L"[aiui] OnHostPaint %dx%d ready=%d err=%s",
               static_cast<int>(rc.right), static_cast<int>(rc.bottom),
               web_.Ready() ? 1 : 0,
               error_.empty() ? L"(none)" : error_.c_str());
    }
    if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(host_, &ps); return; }

    // 占位页：底色 + 说明文字。色彩取自 Theme（面板是主窗口的一块，
    // 深浅色切换时必须跟着变），但**文字色不直接用 theme_->text**：
    // 占位页的底色是 panelBg，而 theme_->text 是"内容区背景上的文字色"，
    // 两者不是一对。实测浅色主题下 panelBg=#FCFBFB、text 也很浅时，
    // 画出来是"白底上白字" —— 用户看到的就是一块空白（真发生过）。
    // 所以这里按**实际对比度**决定文字色，而不是相信主题把两个值配对了。
    // 【底色刻意不用 listBg / panelBg】两者在浅色主题下都是接近纯白
    // （listBg=0xFFFFFF、panelBg=0xFBFBFC），而文件视图的白底也是 0xFFFFFF ——
    // 面板展开后只是"多出一块白"，肉眼无法判断面板是否存在（用户的原话是
    // "位置上还是空的"）。这不是配色偏好问题，是**可辨认性**问题：
    // 一块没有边界的白区，用户只能靠"文件区变窄了"去推断，而那个推断
    // 很容易被当成窗口自己变了。
    //
    // 取"主背景压暗一档"的做法：与文件区有明确色差，又不至于像另一个程序。
    // 用 MixColor 从 bg 往 text 方向混 8%，保持同一色系（浅色 → 极浅灰，
    // 深色 → 稍亮），跨主题都成立，且不新增颜色令牌。
    const COLORREF base = theme_ ? theme_->bg : RGB(32, 32, 32);
    const COLORREF ink = theme_ ? theme_->text : RGB(240, 240, 240);
    const COLORREF bg = ui::MixColor(ink, base, 8);
    // 用 ui::EnsureContrast（现有唯一实现）：它按对比度比值校正，而不是靠
    // "主题应该把两个值配好"这种假设。浅色主题下白底白字就是这么来的。
    const COLORREF fg = ui::EnsureContrast(
        theme_ ? theme_->text : RGB(200, 200, 200),
        bg,
        4.5);
    const HFONT font = theme_ ? theme_->font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    ui::FillRectColor(dc, rc, bg);

    // 把实际用的颜色与字体落盘。占位页"画了但看不见"这类问题，
    // 只靠肉眼看截图无法区分是"没画"还是"画成同色" —— 数值是唯一答案。
    LogDbg(L"[aiui] 占位页绘制 bg=0x%06X fg=0x%06X font=%p theme=%p",
           static_cast<unsigned>(bg), static_cast<unsigned>(fg),
           static_cast<void*>(font), static_cast<const void*>(theme_));

    // ---- 标题条：面板的"我就是 AI 助手"标识 ----
    //
    // 【为什么必须有】面板与文件区并排，光靠底色差一档仍然可能被当成
    // "窗口自己留白"。一条带文字的标题条是**唯一不需要推断**的标识：
    // 看到"AI 助手"三个字，就知道这块是什么、有没有在跑。
    //
    // 这一条与 WebView2 是否可用无关 —— 它由我们自绘，永远可见。
    // 白屏时它同时是排查入口（右侧带端口与状态）。
    {
        const int barH = 30;
        RECT bar{rc.left, rc.top, rc.right, rc.top + barH};
        const COLORREF barBg = ui::MixColor(ink, base, 14);   // 比正文底再深一档
        ui::FillRectColor(dc, bar, barBg);
        // 下缘一条细线，把标题条和内容分开（与栏之间的 separator 同一手法）。
        const RECT barLine{rc.left, rc.top + barH - 1, rc.right, rc.top + barH};
        ui::FillRectColor(dc, barLine, theme_ ? theme_->line : RGB(120, 120, 120));

        RECT tr = bar;
        tr.left += 12;
        tr.right -= 12;
        const COLORREF barFg = ui::EnsureContrast(
            theme_ ? theme_->text : RGB(240, 240, 240), barBg, 4.5);
        ui::DrawTextRow(dc, font, L"AI 助手", tr, barFg,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // 右侧状态：能用就显示端口（一眼看出服务活着），不能用就显示"不可用"。
        const std::wstring badge =
            web_.Ready() ? L"已连接"
            : (servicePort_ ? (L"服务 :" + std::to_wstring(servicePort_)) : L"未启动");
        RECT br = bar;
        br.left += 12;
        br.right -= 12;
        ui::DrawTextRow(dc, font, badge, br, ui::MixColor(barFg, barBg, 70),
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // 文案按"能不能行动"分级，而不是统一一句"正在启动"：
    //   · 有 error_    → 不可用 + 原因（用户能照着修）
    //   · WebView2 已起 → 正在加载页面（此时是 WebView2 自己的白屏，正常）
    //   · 还没发起创建  → 准备中
    std::wstring text;
    if (!error_.empty()) {
        text = L"AI 助手不可用\r\n\r\n" + error_;
    } else if (web_.Ready()) {
        text = L"AI 助手已启动，正在加载界面…";
    } else if (webStartAttempted_) {
        text = L"AI 助手正在创建浏览器视图…";
    } else {
        text = L"AI 助手准备中…";
    }

    // 【退路】浏览器视图起不来（或还没起来）时，把 node 服务的地址写出来。
    //
    // 这是有意的：服务与 WebView2 是两件独立的事 —— 服务（node）通常好好的，
    // 只有内嵌浏览器视图会因运行时/回调问题起不来。把地址给出去，
    // 用户用系统浏览器打开就能用**同一份** UI（ui/app.js 与面板里加载的完全相同），
    // 不必等内嵌视图修好。这比只显示一句"不可用"有用得多。
    if (!web_.Ready() && servicePort_ != 0) {
        wchar_t line[192] = L"";
        const std::wstring httpToken = aisvc::HttpToken();
        swprintf(line, 192, L"\r\n\r\n也可在浏览器打开：\r\nhttp://127.0.0.1:%u/?exbarHttpToken=%s",
                 servicePort_, httpToken.c_str());
        text += line;
    }

    // 第二段：一行诊断。以前占位页只有一句话，白屏时无法区分
    // "服务没起 / WebView2 没起 / 页面在加载" —— 三种修法完全不同。
    // 这行让人一眼看出卡在哪一步。
    wchar_t diag[256] = L"";
    swprintf(diag, 256,
             L"服务端口 %u · 浏览器视图 %s · 服务%s",
             servicePort_,
             web_.Ready() ? L"已就绪" : (webStartAttempted_ ? L"创建中" : L"未创建"),
             aisvc::Enabled() ? L"已启动" : L"未启动");

    // 左侧边界由 aiEdge_（与面板同父的 SplitBar）统一绘制。
    // 这里曾经自用 paneSep 画 2px 竖线：它与边界条的中线叠在一起，正是
    // "AI 边界一条粗一条细 / 上下颜色不一致"的直接来源。现在面板只画自己的
    // 内容，边界的命中、悬停、颜色、厚度全归 SplitBar 一处。

    // 【排版】顶部左对齐，不用 DT_VCENTER。
    // DT_VCENTER 只对**单行**文本有效；与 DT_WORDBREAK 同用时垂直居中失效，
    // 文本位置由 DrawText 内部按行数推算，常把内容推到客户区外 ——
    // 面板很窄（320~420 DIP）、文字又长，这条必踩。
    //
    // 【按 \r\n 拆段逐行画】DrawTextW **不把 \r\n 当换行**（它当普通字符跳过或
    // 画成方块），而我们的文案用 \r\n\r\n 分了段。整段一次性交给 DrawText，
    // 结果是"所有字挤成一行、再被右边界裁掉" —— 实测就是一条横向条纹。
    // 拆段后用单行矩形（DT_SINGLELINE）逐段下移，位置完全可控。
    RECT area = rc;
    area.left += 20;
    area.right -= 20;
    area.top += 30 + 16;   // 让开标题条（30）再留一段呼吸空间

    int y = area.top;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos
                                                 ? std::wstring::npos
                                                 : nl - pos);
        // 去掉行尾残留的 \r
        while (!line.empty() && line.back() == L'\r') line.pop_back();

        if (!line.empty()) {
            RECT lr{area.left, y, area.right, y + 200};
            // 用 ui::MeasureTextHeight 量这一行真实占用的高度（行长会自动折行），
            // 再按量出的高度画。不量就下移固定值，长行会与下一段重叠。
            // （DT_CALCRECT 走 ui_draw.cpp 的唯一实现，见 ui_draw.h 说明。）
            const int h = ui::MeasureTextHeight(dc, font, line, lr,
                                                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            RECT draw = lr;
            draw.bottom = draw.top + h;
            ui::DrawTextRow(dc, font, line, draw, fg,
                            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            y += h + 6;
        } else {
            y += 8;   // 空行：段间留白
        }

        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }

    // 诊断行贴在底部：它与正文用途不同（正文给用户，诊断给排查），
    // 分开放比接在正文后面更容易分辨。
    //
    // 用 DT_SINGLELINE | DT_END_ELLIPSIS 而不是 DT_BOTTOM+DT_WORDBREAK：
    // 诊断串是**单行**（端口/视图/服务三态），给它整块可折行区域会让
    // DT_BOTTOM 按多行推算位置，面板窄时那一行会漂到看不见的地方。
    // 单行 + 省略号是"窄就截断"，这是它有意的行为（截断也还能看出前几个字段）。
    if (!web_.Ready()) {
        RECT dr = rc;
        dr.left += 20;
        dr.right -= 20;
        dr.top = dr.bottom - 34;
        const COLORREF dim = ui::MixColor(fg, bg, 55);   // 诊断行弱化，不抢正文
        ui::DrawTextRow(dc, font, diag, dr, dim,
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS
                            | DT_NOPREFIX);
    }

    // 重试按钮：只有真的失败/卡住才画（见 RetryButtonRect 的判据）。
    // 用描边矩形而不是实心块 —— 面板很窄，实心按钮在浅色主题下像一块补丁。
    RECT btn{};
    if (RetryButtonRect(&btn)) {
        ui::FrameRectColor(dc, btn, ui::MixColor(fg, bg, 70));
        RECT tr2 = btn;
        ui::DrawTextRow(dc, font, L"重试", tr2, fg,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    EndPaint(host_, &ps);
}

void Panel::Destroy() {
    ShutdownWebView();
    if (host_) {
        SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        DestroyWindow(host_);
        host_ = nullptr;
    }
    aisvc::Stop();
    main_ = nullptr;
    visible_ = false;
    hasBounds_ = false;
}

}  // namespace aiui