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
    WM_APP_TAB_COLOR,    // wParam=index, lParam=颜色下标（0=无颜色）。与 MENU 分开：
                         // 一个是"选了哪个动作"，一个是"选了什么值"，混在一个 lParam 要位打包
    WM_APP_TAB_MOVED,    // wParam=from, lParam=to
    WM_APP_CHROME_MENU,  // 通用：wParam=区域 id，lParam=POINT*
    WM_APP_STATUS_MENU,
    WM_APP_BMK_CHANGED,
    WM_APP_BMK_NAV,
    WM_APP_REFOCUS,      // 模态窗口/地址编辑结束后，把焦点还给当前视图
    WM_APP_NAV_REPLAY,   // shell_view 内部：ERROR_BUSY 排队导航的重放

    // Shell 的 WM_PARENTNOTIFY 只有 DOWN、没有配对 UP；用独立消息告诉 InputRouter
    // 派发后立刻释放 clickClaimed_，避免残留 claim 吞掉下一次组件点击。
    WM_APP_MOUSE_DOWN_NOUP = WM_APP + 30,
    WM_APP_MOUSE_UP_NOUP,
};

// 鼠标事件汇入 InputRouter（组件 -> App）。返回 true = 规则已接管，组件跳过默认行为。
// msg 用 WM_LBUTTONDOWN / WM_LBUTTONUP 等原始消息；button 用 mouse_rules.h 的 MB_*。
// 例外：WM_MOUSEWHEEL / WM_MOUSEHWHEEL 的 button 承载滚轮增量；上面的
// WM_APP_MOUSE_*_NOUP 表示无配对抬起的 Shell PARENTNOTIFY 路径。
using MouseSink = std::function<bool(int target, UINT msg, int button, int mods, POINT screen)>;

// 单实例转发（WM_COPYDATA）
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
