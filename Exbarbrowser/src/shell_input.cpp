#include "shell_input.h"
#include "str.h"

#include "mouse_rules.h"
#include "text_file.h"

// SetWindowSubclass / RemoveWindowSubclass（SUBCLASSPROC）与 ListView_GetItem
// 等控件宏都在 commctrl.h 里，且需要 _WIN32_IE >= 0x0500 才导出。
// uiautomation.h 会先把 windows.h 拉进来，导致这里的 commctrl.h 走的是
// 已定型的旧声明集，所以把版本宏在包含之前顶上去，并显式引入。
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <commctrl.h>

#include <uiautomation.h>

#include <cstdlib>
#include <memory>
#include <unordered_map>

namespace shellinput {
namespace {

constexpr UINT_PTR kSubclassId = 0x4558;
constexpr DWORD kUiaCooldownMs = 500;
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

IUIAutomation* Uia() {
    static IUIAutomation* uia = nullptr;
    static ULONGLONG retryAt = 0;   // 0 = 还没试过
    if (uia) return uia;

    // 【必须可重试】UIAutomation 服务是**惰性启动**的，首次 CoCreateInstance
    // 偶发失败是已知现象（本文件其它地方的注释也提到）。原实现用 `static bool tried`
    // 做一次性闩锁：失败后 tried 永真、uia 永为 nullptr，于是整个进程生命周期内
    // 所有 shell 视图的鼠标规则（双击空白上一级、单击项、滚轮）全部退化，
    // 只能重启恢复。
    //
    // 而 DetectViaUia 里那套"失败 → 冷却 2000ms → 再试"只对 ElementFromPoint
    // 失败有效，对**初始化**失败完全无效 —— 这是一条断掉的重试链。
    const ULONGLONG now = GetTickCount64();
    if (retryAt != 0 && now < retryAt) return nullptr;   // 冷却中

    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IUIAutomation, reinterpret_cast<void**>(&uia))) &&
        uia) {
        retryAt = 0;
        return uia;
    }
    uia = nullptr;
    retryAt = now + 5000;   // 5s 后再试：足够跨过服务启动期，又不会频繁重试拖慢输入
    LogDbg(L"[uia] CoCreateInstance 失败，%d ms 后重试", 5000);
    return nullptr;
}

struct BStr {
    BSTR value = nullptr;
    ~BStr() {
        if (value) SysFreeString(value);
    }
    BSTR* Out() { return &value; }
    const wchar_t* Str() const { return value ? value : L""; }
};

ULONGLONG& UiaCooldown() {
    static ULONGLONG until = 0;
    return until;
}

int& UiaFailStreak() {
    static int streak = 0;
    return streak;
}

int ResolveByDir(const std::wstring& dir, const std::wstring& name, bool* isFolder) {
    if (isFolder) *isFolder = false;
    if (name.empty()) return MT_UNKNOWN;
    if (dir.empty() || dir.front() == L':' || dir.rfind(L"shell:", 0) == 0) return MT_UNKNOWN;

    std::wstring full = dir;
    while (!full.empty() && full.back() == L'\\') full.pop_back();
    full += L'\\';
    full += name;

    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(full.c_str(), nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(&item))) &&
        item) {
        SFGAOF attr = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attr)) && (attr & SFGAO_FOLDER)) {
            if (isFolder) *isFolder = true;
            return MT_FOLDER;
        }
        return MT_FILE;
    }
    DWORD attr = GetFileAttributesW(tf::ToExtended(full).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        const bool folder = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isFolder) *isFolder = folder;
        return folder ? MT_FOLDER : MT_FILE;
    }
    return MT_UNKNOWN;
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
    return ResolveByDir(dir, buf, isFolder);
}

int DetectViaUia(HWND hWnd, POINT screen, const std::wstring& dir, std::wstring* name,
                 bool* isFolder) {
    (void)hWnd;
    const ULONGLONG now = GetTickCount64();
    if (now < UiaCooldown()) return MT_UNKNOWN;
    // 冷却结束后给一轮新的探测机会：上一次的失败计数不能带进新周期，
    // 否则 UIA 服务恢复后第一个事件仍会因旧 streak 立刻再次冷却。
    if (UiaCooldown()) {
        UiaCooldown() = 0;
        UiaFailStreak() = 0;
    }

    IUIAutomation* uia = Uia();
    if (!uia) {
        UiaCooldown() = now + 2000;
        LogDbg(L"[shell-input] UIA 初始化失败，冷却 2000ms");
        return MT_UNKNOWN;
    }

    // UIAutomation 首次调用偶发返回失败（服务惰性启动）。同一次探测内立即重试一次，
    // 避免单次抖动就让第一击落空；连续 3 个探测都失败才冷却，防 RPC 风暴。
    ComPtr<IUIAutomationElement> element;
    HRESULT hr = uia->ElementFromPoint(screen, &element);
    if (FAILED(hr) || !element) {
        element.Reset();
        hr = uia->ElementFromPoint(screen, &element);
    }
    if (FAILED(hr) || !element) {
        const int streak = ++UiaFailStreak();
        LogDbg(L"[shell-input] UIA 探测失败 streak=%d", streak);
        if (streak >= 3) UiaCooldown() = now + kUiaCooldownMs;
        return MT_UNKNOWN;
    }
    UiaCooldown() = 0;  // 成功一次就解除冷却
    UiaFailStreak() = 0;

    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(uia->get_ControlViewWalker(&walker)) || !walker) return MT_UNKNOWN;

    IUIAutomationElement* listItem = nullptr;
    bool sawList = false;
    bool sawItemsView = false;
    bool sawGroup = false;
    bool sawColumn = false;
    bool sawTab = false;
    bool sawSearch = false;
    bool sawAddress = false;
    bool sawToolbar = false;

    // 有限上溯：Exbar.cpp 的实测值。跨进程 RPC 很贵，不能逐层走满整棵树。
    IUIAutomationElement* cur = element.get();
    cur->AddRef();
    for (int depth = 0; depth < 6 && cur; ++depth) {
        CONTROLTYPEID type = 0;
        if (SUCCEEDED(cur->get_CurrentControlType(&type))) {
            if (type == UIA_ListControlTypeId) sawList = true;
            if (type == UIA_TabItemControlTypeId) {
                sawTab = true;
                break;
            }
            if (type == UIA_ListItemControlTypeId) {
                if (!listItem) {
                    listItem = cur;
                    listItem->AddRef();
                }
                break;
            }
            if (type == UIA_HeaderItemControlTypeId) {
                sawColumn = true;
                break;
            }
        }

        BStr cls;
        if (SUCCEEDED(cur->get_CurrentClassName(cls.Out()))) {
            const wchar_t* c = cls.Str();
            if (str::IEquals(c, L"UIGroupItem")) sawGroup = true;
            else if (str::IEquals(c, L"UIItemsView")) sawItemsView = true;
            else if (str::IEquals(c, L"BreadcrumbBar") ||
                     str::IEquals(c, L"AddressBar")) sawAddress = true;
            else if (str::IEquals(c, L"SearchBox") ||
                     str::IEquals(c, L"SearchEditBox")) sawSearch = true;
            else if (str::IEquals(c, L"CommandBar") ||
                     str::IEquals(c, L"ToolbarWindow32")) sawToolbar = true;
            if (sawAddress || sawSearch || sawToolbar) break;
        }

        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) break;
        cur->Release();
        cur = parent;
    }
    if (cur) cur->Release();

    int result = MT_UNKNOWN;
    if (sawTab) {
        result = MT_TAB;
    } else if (sawColumn) {
        result = MT_COLUMN;
    } else if (sawSearch) {
        result = MT_SEARCH;
    } else if (sawAddress) {
        result = MT_ADDRESS;
    } else if (sawToolbar) {
        result = MT_TOOLBAR;
    } else if (sawGroup) {
        // Group 容器只记 anchor 不截断：组内空白仍要落到 UIItemsView 判空。
        result = MT_GROUP;
    } else if (listItem) {
        BStr itemName;
        if (SUCCEEDED(listItem->get_CurrentName(itemName.Out())) && itemName.Str()[0]) {
            if (name) *name = itemName.Str();
            result = ResolveByDir(dir, itemName.Str(), isFolder);
        }
        listItem->Release();
        return result;
    } else if (sawItemsView || sawList) {
        result = MT_EMPTY;
    }
    if (listItem) listItem->Release();
    return result;
}

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

    if (ctx->listView) {
        Subclass(ctx->listView, ctx.get());  // Win10
    } else if (ctx->defView) {
        Subclass(ctx->defView, ctx.get());   // Win11 DirectUI：点击经 WM_PARENTNOTIFY 上抛
    }
    if (ctx->tree && ctx->tree != ctx->listView && ctx->tree != ctx->defView)
        Subclass(ctx->tree, ctx.get());

    LogDbg(L"[shell-input] install view=%p container=%p list=%p def=%p tree=%p", viewWindow,
           container, ctx->listView, ctx->defView, ctx->tree);
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
    return DetectViaUia(hWnd, screen, dir, name, isFolder);
}

}  // namespace shellinput
