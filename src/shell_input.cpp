#include "shell_input.h"
#include "str.h"

#include "mouse_rules.h"
#include "text_file.h"

#include "shell_input_uia.h"   // uia::DetectViaUia / uia::ResolveByDir（UIA 分支在另一个 TU）

// SetWindowSubclass / RemoveWindowSubclass（SUBCLASSPROC）与 ListView_GetItem
// 等控件宏都在 commctrl.h 里，且需要 _WIN32_IE >= 0x0500 才导出。
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <commctrl.h>

#include <memory>
#include <unordered_map>

namespace shellinput {
namespace {

constexpr UINT_PTR kSubclassId = 0x4558;
constexpr DWORD kClickDedupMs = 50;

struct Context {
    HWND container = nullptr;
    std::wstring dir;
    MouseSink sink;
    HWND defView = nullptr;
    HWND listView = nullptr;
    HWND tree = nullptr;
};

std::unordered_map<HWND, std::unique_ptr<Context>> g_contexts;

LRESULT CALLBACK ShellSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR idSubclass, DWORD_PTR ref);

bool ClassIs(HWND h, const wchar_t* name) {
    wchar_t cls[64] = L"";
    if (!h || !GetClassNameW(h, cls, _countof(cls))) return false;
    return str::IEquals(cls, name);
}

HWND FindAncestorClass(HWND h, const wchar_t* name, int limit = 16) {
    HWND p = h ? GetParent(h) : nullptr;
    while (p && limit-- > 0) {
        if (ClassIs(p, name)) return p;
        p = GetParent(p);
    }
    return nullptr;
}

void InspectWindow(HWND h, Context* ctx) {
    if (!h || !ctx) return;
    if (ClassIs(h, L"SHELLDLL_DefView")) {
        ctx->defView = h;
        return;
    }
    if (ClassIs(h, L"SysListView32")) {
        ctx->listView = h;
        return;
    }
    if (ClassIs(h, L"DirectUIHWND")) {
        if (HWND def = FindAncestorClass(h, L"SHELLDLL_DefView")) ctx->defView = def;
        return;
    }
    if (ClassIs(h, L"SysTreeView32") &&
        ClassIs(GetParent(h), L"NamespaceTreeControl")) {
        ctx->tree = h;
    }
}

BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    InspectWindow(h, reinterpret_cast<Context*>(lp));
    return TRUE;
}

bool Subclass(HWND h, Context* ctx) {
    if (!h || !ctx || !IsWindow(h)) return false;
    return SetWindowSubclass(h, ShellSubclassProc, kSubclassId,
                             reinterpret_cast<DWORD_PTR>(ctx)) != FALSE;
}

void Unsubclass(HWND h) {
    if (h && IsWindow(h)) RemoveWindowSubclass(h, ShellSubclassProc, kSubclassId);
}

int DetectViaListView(HWND hWnd, POINT screen, const std::wstring& dir, std::wstring* name,
                      bool* isFolder) {
    POINT client = screen;
    ScreenToClient(hWnd, &client);
    LVHITTESTINFO ht{};
    ht.pt = client;
    const int idx = ListView_SubItemHitTest(hWnd, &ht);
    if (idx < 0 || (ht.flags & LVHT_NOWHERE)) return MT_EMPTY;

    // 512 而不是 MAX_PATH：这里装的是**单个文件名**（NTFS 上限 255 字符），
    // MAX_PATH(260) 刚好够但没余量 —— 一旦哪个文件系统允许多几个字符，
    // ListView 会静默截断，ResolveByDir 拼出错误路径，鼠标规则对该项失效。
    wchar_t buf[512] = L"";
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = idx;
    it.pszText = buf;
    it.cchTextMax = _countof(buf);
    if (!ListView_GetItem(hWnd, &it) || !buf[0]) return MT_UNKNOWN;
    if (name) *name = buf;
    return uia::ResolveByDir(dir, buf, isFolder);
}

// UIA 分支（Uia / BStr / UiaCooldown / UiaFailStreak / ResolveByDir / DetectViaUia）
// 整体在 shell_input_uia.cpp：Win11 DirectUI 视图没有可 hit-test 的列表控件，
// 只能问 UIAutomation。这里只保留 ListView 路线与鼠标分派。

int ModsFrom(WPARAM w) {
    if ((GetKeyState(VK_RMENU) & 0x8000) && (GetKeyState(VK_LCONTROL) & 0x8000)) return 0;
    int mods = 0;
    if ((w & MK_CONTROL) || (GetKeyState(VK_CONTROL) & 0x8000)) mods |= 1;
    if ((w & MK_SHIFT) || (GetKeyState(VK_SHIFT) & 0x8000)) mods |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= 4;
    return mods;
}

int ButtonFrom(UINT msg, WPARAM w) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return MBTN_LEFT;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return MBTN_RIGHT;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return MBTN_MIDDLE;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: {
        DWORD x = HIWORD(w);
        if (!x) {  // WM_PARENTNOTIFY 不带侧键编号，用物理状态兜底
            const bool x1 = (GetKeyState(VK_XBUTTON1) & 0x8000) != 0;
            const bool x2 = (GetKeyState(VK_XBUTTON2) & 0x8000) != 0;
            if (x1 && !x2) x = XBUTTON1;
            else if (x2 && !x1) x = XBUTTON2;
        }
        if (x == XBUTTON1) return MBTN_X1;
        if (x == XBUTTON2) return MBTN_X2;
        return -1;
    }
    default: return -1;
    }
}

bool IsDown(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
           msg == WM_XBUTTONDOWN;
}

bool IsDbl(UINT msg) {
    return msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK ||
           msg == WM_XBUTTONDBLCLK;
}

bool IsUp(UINT msg) {
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP ||
           msg == WM_XBUTTONUP;
}

// 同一物理点击可能同时以直接消息和 WM_PARENTNOTIFY 上抛；50ms 内同窗口同键
// 只算一次。DBLCLK 是真实的第二击，不能被去重吞掉（Exbar.cpp 的实测修正）。
bool IsDuplicateDown(HWND hWnd, int button, UINT msg) {
    static thread_local HWND lastHwnd[MBTN_COUNT] = {};
    static thread_local DWORD lastTime[MBTN_COUNT] = {};
    if (button < 0 || button >= MBTN_COUNT || IsDbl(msg)) return false;
    const DWORD now = GetTickCount();
    const bool dup = lastHwnd[button] == hWnd && (now - lastTime[button]) < kClickDedupMs;
    lastHwnd[button] = hWnd;
    lastTime[button] = now;
    return dup;
}

Context* FindContext(HWND container) {
    auto it = g_contexts.find(container);
    return it == g_contexts.end() ? nullptr : it->second.get();
}

LRESULT CALLBACK ShellSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR idSubclass, DWORD_PTR ref) {
    if (uMsg == WM_NCDESTROY) {
        Unsubclass(hWnd);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    auto* ctx = reinterpret_cast<Context*>(ref);
    if (!ctx || !ctx->sink || FindContext(ctx->container) != ctx) {
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    UINT childMsg = uMsg;
    bool parentNotify = false;
    if (uMsg == WM_PARENTNOTIFY) {
        childMsg = LOWORD(wParam);
        parentNotify = true;
    }

    if (uMsg == WM_MOUSEMOVE) {
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ctx->sink(MT_UNKNOWN, uMsg, MBTN_LEFT, ModsFrom(wParam), screen);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        if (!MouseRules::Get().HasWheelRules())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // 滚轮消息本来就是屏幕坐标
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int target = DetectTarget(hWnd, screen, ctx->dir);
        bool consumed = ctx->sink(target, uMsg, delta, ModsFrom(wParam), screen);
        return consumed ? 0 : DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    if (!parentNotify && !IsDown(childMsg) && !IsDbl(childMsg) && !IsUp(childMsg))
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    const int button = parentNotify ? ButtonFrom(childMsg, 0) : ButtonFrom(childMsg, wParam);
    if (button < 0 || !MouseRules::Get().HasPointerRules(button))
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    if ((IsDown(childMsg) || IsDbl(childMsg)) && IsDuplicateDown(hWnd, button, childMsg))
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    POINT screen{};
    if (parentNotify) {
        GetCursorPos(&screen);
    } else if (IsUp(childMsg)) {
        // UP 的坐标也是客户区；UP 不改目标，直接沿用当前光标位置即可。
        GetCursorPos(&screen);
    } else {
        POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        screen = client;
        ClientToScreen(hWnd, &screen);
    }

    // DirectUI 的点击只经 WM_PARENTNOTIFY 上抛，DOWN 也必须做命中测试；
    // 否则两次点击都是 unknown，规则引擎永远拿不到 empty/file/folder。
    // UP 不参与目标判定，沿用 unknown，由 InputRouter 用 pending 目标兜底。
    const int target = IsUp(childMsg) ? MT_UNKNOWN : DetectTarget(hWnd, screen, ctx->dir);
    if (!IsUp(childMsg)) {
        LogDbg(L"[shell-input] DOWN parent=%d btn=%d target=%s pt=%d,%d", parentNotify ? 1 : 0,
               button, MouseTargetName(target), screen.x, screen.y);
    }
    UINT sinkMsg = childMsg;
    if (parentNotify) {
        if (IsDown(childMsg)) sinkMsg = WM_APP_MOUSE_DOWN_NOUP;
        else if (IsUp(childMsg)) sinkMsg = WM_APP_MOUSE_UP_NOUP;
    }
    const bool consumed = ctx->sink(target, sinkMsg, button, ModsFrom(wParam), screen);

    // 左键第一次按下要放行：否则双击文件/文件夹规则会让第一击连"选中"都没有。
    // 只有单击/长按规则才需要吞掉第一击；纯双击规则等第二击（DBLCLK）再吞。
    if (!parentNotify && consumed && childMsg == WM_LBUTTONDOWN) {
        const MouseRules& rules = MouseRules::Get();
        const bool onlyDouble = rules.HasTriggerRules(button, TR_DOUBLE) &&
                                !rules.HasTriggerRules(button, TR_SINGLE) &&
                                !rules.HasTriggerRules(button, TR_HOLD);
        if (onlyDouble) return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
    if (!parentNotify && consumed) return 0;
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

}  // namespace

bool Install(HWND viewWindow, HWND container, const std::wstring& dir, MouseSink sink) {
    if (!viewWindow || !container) return false;
    Uninstall(container);

    auto ctx = std::make_unique<Context>();
    ctx->container = container;
    ctx->dir = dir;
    ctx->sink = std::move(sink);

    InspectWindow(viewWindow, ctx.get());
    EnumChildWindows(viewWindow, FindProc, reinterpret_cast<LPARAM>(ctx.get()));

    // 【必须判返回值】Subclass 失败 = 这个窗口没挂上子类，鼠标规则（双击空白
    // 上一级 / 单击项 / 滚轮）在这个视图上全部静默失效。原来三处返回值全丢，
    // 只要有一个挂上就 return true，调用方（OnViewCreated）以为装好了，
    // 用户只会觉得"鼠标规则时灵时不灵"。
    int hooked = 0;
    if (ctx->listView) {
        hooked += Subclass(ctx->listView, ctx.get()) ? 1 : 0;   // Win10
    } else if (ctx->defView) {
        hooked += Subclass(ctx->defView, ctx.get()) ? 1 : 0;    // Win11 DirectUI：点击经 WM_PARENTNOTIFY 上抛
    }
    if (ctx->tree && ctx->tree != ctx->listView && ctx->tree != ctx->defView)
        hooked += Subclass(ctx->tree, ctx.get()) ? 1 : 0;

    LogDbg(L"[shell-input] install view=%p container=%p list=%p def=%p tree=%p hooked=%d",
           viewWindow, container, ctx->listView, ctx->defView, ctx->tree, hooked);
    if (hooked == 0) {
        // 一个都没挂上：不登记上下文，让调用方拿到 false 去记一条明确失败，
        // 而不是带着一个"看起来装好了、实际不工作"的假状态继续跑。
        LogDbg(L"[shell-input] install 失败：没有任何窗口可挂子类（Win 版本层级变化？）");
        return false;
    }
    g_contexts[container] = std::move(ctx);
    return true;
}

void Uninstall(HWND container) {
    auto it = g_contexts.find(container);
    if (it == g_contexts.end()) return;
    Unsubclass(it->second->listView);
    Unsubclass(it->second->defView);
    Unsubclass(it->second->tree);
    g_contexts.erase(it);
}

void UpdateDir(HWND container, const std::wstring& dir) {
    if (Context* ctx = FindContext(container)) ctx->dir = dir;
}

int DetectTarget(HWND hWnd, POINT screen, const std::wstring& dir, std::wstring* name,
                 bool* isFolder) {
    if (name) name->clear();
    if (isFolder) *isFolder = false;

    wchar_t cls[64] = L"";
    if (GetClassNameW(hWnd, cls, _countof(cls))) {
        if (str::IEquals(cls, L"SysTreeView32")) return MT_TREE;
        if (str::IEquals(cls, L"SysListView32"))
            return DetectViaListView(hWnd, screen, dir, name, isFolder);
    }
    return uia::DetectViaUia(hWnd, screen, dir, name, isFolder);
}

}  // namespace shellinput
