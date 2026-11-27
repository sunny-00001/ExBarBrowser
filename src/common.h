// ExBarBrowser - 公共定义（唯一共享头，只放类型/常量/纯内联工具）
//
// 设计纪律（沿用 ExBar 工程）：
//   · 一个文件一个职责；本文件只做"语言级"共享，不含业务。
//   · 每个实现文件 <=500 行；跨文件只经显式接口。
//   · 独立 EXE 不受 Windhawk 单 TU 约束：真多文件、真命名空间。
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <knownfolders.h>

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 路径语义的唯一实现（父级/叶子/盘根/向上目标）。本文件只做名字转发。
#include "nav_path.h"

// 日志的唯一实现（%TEMP%\ExBarBrowser.log + 轮转）。
// 以前这里有一份 inline LogDbg、shell_view.cpp 里另有一份 LogLine，两份轮转策略
// 还不一致（一份按大小、一份按行数）。2026-11-20 已合并到 log.h 的 LogLine/LogDbg。
#include "log.h"

// 字符串工具的唯一实现（Trim / Lower / ICmp / UTF-8 转换）。
#include "str.h"

// SHELLSTATE 掩码（mingw 的 shlobj.h 只声明了结构体，没有 SSF_* 常量）
#ifndef SSF_SHOWALLOBJECTS
#define SSF_SHOWALLOBJECTS 0x00000001
#endif
#ifndef SSF_SHOWEXTENSIONS
#define SSF_SHOWEXTENSIONS 0x00000002
#endif
#ifndef SSF_SHOWSUPERHIDDEN
#define SSF_SHOWSUPERHIDDEN 0x00040000
#endif

// 品牌串、窗口类名、进程级资源名的唯一来源（namespace id）。
// 以前这些字面量散在 9 个文件里，改名要动 9 处；现在只改 identity.h。
//   · kMainClass / kTabStripClass / ... → 见 identity.h
//   · kCopyDataMagic                    → 见 identity.h
#include "identity.h"

using id::kMainClass;
using id::kViewAreaClass;
using id::kTabViewClass;
using id::kTabStripClass;
using id::kNavBarClass;
using id::kStatusBarClass;

// 子窗口 -> 主窗口的自定义消息（避免跨文件依赖 App 头）
enum : UINT {
    WM_APP_TAB_SELECT = WM_APP + 10,
    WM_APP_TAB_CLOSE,
    WM_APP_TAB_NEW,
    WM_APP_TAB_MENU,     // wParam=index, lParam=screen POINT 打包（LOWORD x / HIWORD y 不可靠，用 lParam 指针）
    WM_APP_TAB_MOVED,    // wParam=from, lParam=to
    WM_APP_TAB_TEAROFF,  // wParam=index：拖出栏体松手 → 移到新窗口（tab.move.window）
    WM_APP_CHROME_MENU,  // 通用：wParam=区域 id，lParam=POINT*
    WM_APP_STATUS_MENU,
    WM_APP_BMK_CHANGED,
    WM_APP_BMK_NAV,
    WM_APP_REFOCUS,      // 模态窗口/地址编辑结束后，把焦点还给当前视图
    WM_APP_NAV_REPLAY,   // shell_view 内部：ERROR_BUSY 排队导航的重放
    // AI 通道热插拔：config.set aiEnabled 之后由 UI 线程真正启停服务。
    // 必须走"投递到消息队列"而不是在命令 handler 里直接调 StopServer ——
    // 关通道的请求本身就可能来自该通道，同步 join 监听线程会让响应写不回去。
    WM_APP_SYNC_AI,

    // Shell 的 WM_PARENTNOTIFY 只有 DOWN、没有配对 UP；用独立消息告诉 InputRouter
    // 派发后立刻释放 clickClaimed_，避免残留 claim 吞掉下一次组件点击。
    WM_APP_MOUSE_DOWN_NOUP = WM_APP + 30,
    WM_APP_MOUSE_UP_NOUP,
    // 焦点事件观察（EVENT_OBJECT_FOCUS）回调里回投 UI 线程：双栏下焦点换了视图，
    // 据此同步活动窗格与外壳（地址栏/标签/状态栏）。只读观察，不改变焦点。
    WM_APP_FOCUS_CHANGED = WM_APP + 40,

    // 导航根集的**发现**在工作线程上跑完了（见 nav_pane_root.h 的 Prefetch*）：
    // 回到 UI 线程把根集换成完整清单。
    // 【为什么必须是独立消息而不是 Sleep 轮询】轮询会把 UI 线程拖住，
    // 正是本次要修的"启动半秒空白"。这里由工作线程 PostMessage，UI 线程
    // 一有空就处理；lParam 不用（清单本身在 nav_pane_root 的缓存里）。
    WM_APP_NAV_ROOTS_READY = WM_APP + 50,

    // 首帧已经画出来了：启动期押后的活（建右窗格视图）现在补。
    //
    // 【为什么不在 Run 里直接调】要让首帧先落到屏幕上，必须让消息循环开始转
    // （ShowWindow + UpdateWindow 之后 WM_PAINT 才被处理）。所以这里投一条消息：
    // 它的处理时刻"必然在首帧之后"，比在 Run 里调更早不了、也更晚不了。
    WM_APP_STARTUP_SETTLE = WM_APP + 51,

    // AI 面板：把"创建 WebView2"推迟到消息循环空闲时。
    //
    // 【为什么必须推迟】WebView2 的环境/控制器创建是异步的，回调靠本线程的
    // 消息循环派发。从命令处理器里同步调用 Start()，那一帧消息循环还没转回来，
    // 回调无处可送 —— 实测表现是 CreateCoreWebView2EnvironmentWithOptions 返回
    // S_OK，然后一片安静，永远等不到 OnEnvironmentReady。
    // 投一条消息，处理时刻必然在循环空闲之后，回调就能正常送到。
    WM_APP_AI_PANEL_CREATE = WM_APP + 52,

    // 关闭的标签页：视图已从界面摘掉，真正销毁推迟到这里。
    //
    // 【为什么延后】探针 84 实测 ShellView::Destroy（IExplorerBrowser::Destroy +
    // Release + DestroyWindow）= 20~69 ms per view，全在 UI 线程。关一个标签会
    // 同步销毁 1~2 个视图，于是"点 X 到界面有反应"要 20~138 ms。
    // 用户对"关闭"的期待是立刻消失，清理可以稍后做 —— 见 app.h 的 pendingDestroy_。
    WM_APP_RECLAIM_VIEWS = WM_APP + 53,

    // AI 面板：WebView2 就绪并可见后，投一条消息做一次"尺寸轻推"来强制呈现。
    //
    // 【为什么需要】WebView2 在父窗口已是最终尺寸时首次创建，内容会在内部
    // 渲染好，但合成器不会把它呈现出来 —— 面板一片纯白（实测取样 100% 白）。
    // 只要父窗口尺寸发生一次真实变化，合成器立刻重建可见表面并完成呈现
    // （取样从 1 种颜色变成上百种）。这是 WebView2Feedback #5673 记录的行为。
    // 就绪回调里 put_Bounds 用的是和已有值相同的矩形，不构成尺寸变化，
    // 所以必须在稍后的消息循环里把宿主缩小 2px 再还原，制造一次真实变化。
    WM_APP_AI_WEBVIEW_PRESENT = WM_APP + 54,

    // 导航窗格空白区右键：NSTC 的事件回调里不能跑模态弹层（会重入 shell），
    // 由占位 IContextMenu 投这条消息，回到正常消息循环后走 ui::Menu::Show。
    // lParam = 鼠标屏幕坐标（MAKEPOINT），wParam 不用。
    WM_APP_NAV_BLANK_MENU = WM_APP + 55,

    // 文件视图自动刷新（M7）：shell 变更通知从 NavPane 的宿主窗口回投到主窗口。
    // 【为什么投消息而不是 NavPane 直接调 App】依赖方向是 app → nav_pane，
    // 下层不能反向 include 上层；投消息让 App 自己决定何时刷哪个窗格。
    // App 侧用 300ms 合并定时器去抖：批量文件操作会连发几十条通知，只刷一次。
    WM_APP_VIEW_AUTO_REFRESH = WM_APP + 56,
};

// 鼠标事件汇入 InputRouter（组件 -> App）。返回 true = 规则已接管，组件跳过默认行为。
// msg 用 WM_LBUTTONDOWN / WM_LBUTTONUP 等原始消息；button 用 mouse_rules.h 的 MB_*。
// 例外：WM_MOUSEWHEEL / WM_MOUSEHWHEEL 的 button 承载滚轮增量；上面的
// WM_APP_MOUSE_*_NOUP 表示无配对抬起的 Shell PARENTNOTIFY 路径。
using MouseSink = std::function<bool(int target, UINT msg, int button, int mods, POINT screen)>;

// 路径转发（WM_COPYDATA）。已有窗口收到后在新标签里打开；没有窗口则调用方新开进程。
//
// 【跨软件契约：magic 与 path 的位置/大小永不改】
// /select 要选中的目标作为**变长尾随数据**跟在 path 的 NUL 之后，不占结构字段：
// 发送端按"结构头 + path 实际字符 + 可选 select 实际字符"分配连续缓冲，
// 接收端先读 path 到 NUL，再从 NUL 之后按 cbData 剩余长度读 select。
// 这样新→老（老实例只读 path、忽略尾部）与老→新（cbData 只够 path、无 select）
// 都天然兼容 —— 这正是接收端"按 cbData 实际长度读"设计的本意。
using id::kCopyDataMagic;
struct CopyDataPayload {
    ULONG_PTR magic;
    wchar_t path[1024];
};

// 最小 ComPtr：只覆盖本项目用到的语义（不提供拷贝，避免误共享）。
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    T** operator&() {
        Reset();
        return &p_;
    }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

// 图标真实像素宽（SHGFI_SMALLICON 给的是 16/20 位图；强行按 S(16) 拉伸会虚）
inline int IconPx(HICON icon, int fallback = 16) {
    if (!icon) return fallback;
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return fallback;
    BITMAP bm{};
    int w = fallback;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0) w = bm.bmWidth;
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return w;
}

inline int DpiOf(HWND h) {
    static auto fn = reinterpret_cast<UINT(WINAPI*)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT d = fn ? fn(h) : 96;
    return d ? static_cast<int>(d) : 96;
}

// PIDL -> 可显示/可再解析的路径（文件系统返回 C:\...，命名空间返回 ::{GUID}）
inline std::wstring PidlName(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return std::wstring();
    PWSTR name = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
        std::wstring s(name);
        CoTaskMemFree(name);
        return s;
    }
    return std::wstring();
}

// ---------------------------------------------------------------- 路径语义
//
// 唯一的实现在 nav_path.h（纯字符串、无 MAX_PATH 限制、可单测）。
// 这里只做**转发**，保留旧名字给既有调用点用 —— 不保留第二份实现：
// 以前 common.h 与 NavModel 各有一套 ParentOf/LeafOf，规则不一致导致
// "上一级"和"地址栏补全"算出不同结果，超长路径还会静默失效。
inline bool IsDriveRoot(const std::wstring& p) { return nav::IsDriveRoot(p); }
inline std::wstring ParentOf(const std::wstring& p) { return nav::ParentOf(p); }
inline std::wstring LeafOf(const std::wstring& p) { return nav::Leaf(p); }
inline bool CanNavigateUp(const std::wstring& p) { return nav::CanNavigateUp(p); }

// 桌面命名空间的根（"此电脑"）—— 保留旧名，实现只有 nav::kThisPc 一份。
inline constexpr const wchar_t* kThisPcPath = nav::kThisPc;

inline std::wstring CountText(unsigned n) {
    return std::to_wstring(n) + L" 个项目";
}

// 清历史（"只剩当前目录一条"）的唯一实现在 nav::History::ClearKeepingHere()。
// 以前这里是自由函数 CollapseHistoryToCurrent，与 App::ClearHistory 各写一遍规则。

inline void SetThis(HWND h, void* p) {
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
}

template <class T>
inline T* GetThis(HWND h) {
    return reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

// 用注册表读系统深浅色（与 ExBarUI/ThemeSink 同口径：explorer 跟随"Windows 模式"）
inline bool SystemIsDark() {
    DWORD v = 1, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) {
        return v == 0;
    }
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) {
        return v == 0;
    }
    return true;
}

// Win11 深色标题栏：20 是正式属性号，1809 用 19，逐个尝试。
inline void ApplyDarkTitleBar(HWND h, bool dark) {
    BOOL on = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &on, sizeof(on))))
        DwmSetWindowAttribute(h, 19, &on, sizeof(on));
}
