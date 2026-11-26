// Shell 视图命中探测的 UIA 分支（Win11 DirectUI 视图）。分段理由见
// shell_input_uia.h：让 UIA 在坏掉时不拖垮输入的那套重试/冷却/失败计数，
// 与"元素祖先链 → MouseTarget"的映射放在一起，而鼠标分派留在 shell_input.cpp。
#include "shell_input_uia.h"

#include "mouse_rules.h"
#include "str.h"
#include "text_file.h"

// uiautomation.h 会先把 windows.h 拉进来；先顶版本宏再显式包含（同 shell_input.cpp）。
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <uiautomation.h>

namespace shellinput {
namespace uia {
namespace {

constexpr DWORD kUiaCooldownMs = 500;

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




}  // namespace

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

}  // namespace uia
}  // namespace shellinput
