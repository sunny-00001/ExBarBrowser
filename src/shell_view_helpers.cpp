#include "shell_view_helpers.h"

// 类名比较只用于 shell 宿主链上的固定类名；大小写无关按 Windows 类名语义。
bool ClassIsName(HWND h, const wchar_t* name) {
    if (!h) return false;
    wchar_t cls[64] = L"";
    if (!GetClassNameW(h, cls, _countof(cls))) return false;
    return wcscmp(cls, name) == 0;
}

// 找 parent 子树里第一个类名为 name 的窗口（深度优先，命中即停）。
HWND FindChildByClass(HWND parent, const wchar_t* name) {
    struct Ctx {
        const wchar_t* name = nullptr;
        HWND found = nullptr;
    } ctx;
    if (!parent) return nullptr;
    ctx.name = name;
    EnumChildWindows(
        parent,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (ClassIsName(h, c->name)) {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}
