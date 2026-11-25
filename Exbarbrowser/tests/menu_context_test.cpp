// 统一菜单的 Shell 桥接测试：NSTC 交回的 IContextMenu 仍必须由 ui::Menu 供内容。
//
// 这里同时验三件容易散掉的事：
//   1. owner-draw 行来自 ui::Menu（图标/文案不退化成调用点 AppendMenuW）；
//   2. 命令回调拿的是 ui::Menu 的 action，不是行号；
//   3. 快捷键在 QueryContextMenu 现取（换键位表后同一份菜单量宽变化）。
#include <cstdio>
#include <cwchar>

#include "menu.h"
#include "menu_context.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int MeasuredWidth(ui::ShellContextMenu* ctx, UINT row) {
    MEASUREITEMSTRUCT measure{};
    measure.CtlType = ODT_MENU;
    measure.itemData = row;
    ctx->HandleMenuMsg(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&measure));
    return static_cast<int>(measure.itemWidth);
}

// 故意给一个不等于行号的 itemID：原生菜单传的是 wID，桥接层必须按 itemData
// 重映射，才能把 row 交给与书签菜单共用的 PopupDrawItem。
COLORREF DrawnPixel(ui::ShellContextMenu* ctx, HDC dc, UINT row, bool selected,
                    const RECT& rc) {
    PatBlt(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, WHITENESS);
    DRAWITEMSTRUCT draw{};
    draw.CtlType = ODT_MENU;
    draw.hDC = dc;
    draw.rcItem = rc;
    draw.itemID = 500 + row;
    draw.itemData = row;
    draw.itemState = selected ? ODS_SELECTED : 0;
    if (FAILED(ctx->HandleMenuMsg(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw))))
        return CLR_INVALID;
    return GetPixel(dc, rc.left + 1, rc.top + 1);
}

}  // namespace

int main() {
    std::printf("统一菜单的 Shell 上下文桥接\n");

    Theme theme;
    theme.Init(nullptr, 96);

    ui::Menu menu;
    menu.Add(L"刷新", 11, ico::IconId::Refresh).Cmd(CmdId::NavRefresh);
    menu.Add(L"展开所有文件夹", 12, ico::IconId::Folder).Cmd(CmdId::NavTreeExpandAll);

    ui::Menu::SetShortcutResolver(
        [](CmdId id, const std::wstring&) {
            return id == CmdId::NavTreeExpandAll ? std::wstring(L"Ctrl+Shift+E")
                                                 : std::wstring();
        },
        nullptr);

    int invoked = -1;
    auto* ctx = new ui::ShellContextMenu(menu, theme, [&](int action) {
        invoked = action;
        return true;
    });
    HMENU popup = CreatePopupMenu();
    const HRESULT hr = ctx->QueryContextMenu(popup, 0, 100, 110, CMF_NORMAL);
    Check(SUCCEEDED(hr) && HRESULT_CODE(hr) == 2, "Shell 菜单行数来自 ui::Menu");

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE | MIIM_STRING;
    wchar_t caption[64] = L"";
    info.dwTypeData = caption;
    info.cch = 64;
    Check(GetMenuItemInfoW(popup, 0, TRUE, &info) && (info.fType & MFT_OWNERDRAW) &&
              std::wstring(caption) == L"刷新",
          "图标行按统一菜单内容生成 owner-draw 项，文案来自 ui::Menu");

    const int withKeyWidth = MeasuredWidth(ctx, 1);
    Check(withKeyWidth > 0, "owner-draw 行能正常量宽");

    MEASUREITEMSTRUCT height{};
    height.CtlType = ODT_MENU;
    height.itemData = 1;
    ctx->HandleMenuMsg(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&height));
    Check(height.itemHeight == static_cast<UINT>(theme.S(24)),
          "行高与书签菜单同一令牌 S(24)，不取 SM_CYMENU");

    // 底色/高亮直接读回像素：桥接层必须走 PopupDrawItem，而不是自己 GetSysColor。
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 400, 64);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    const RECT rowRc{0, 0, 400, theme.S(24)};
    Check(DrawnPixel(ctx, mem, 1, false, rowRc) == theme.listBg,
          "普通底色来自 Theme::listBg，与书签菜单同源");
    Check(DrawnPixel(ctx, mem, 1, true, rowRc) == theme.listSel,
          "选中高亮来自 Theme::listSel，与书签菜单同源");
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    CMINVOKECOMMANDINFO invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.lpVerb = MAKEINTRESOURCEA(1);
    Check(SUCCEEDED(ctx->InvokeCommand(&invoke)) && invoked == 12,
          "菜单动作回调拿 ui::Menu 的 action，不是行号");
    DestroyMenu(popup);
    ctx->Release();

    ui::Menu::SetShortcutResolver(nullptr, nullptr);
    auto* noKey = new ui::ShellContextMenu(menu, theme, [](int) { return true; });
    HMENU popup2 = CreatePopupMenu();
    noKey->QueryContextMenu(popup2, 0, 100, 110, CMF_NORMAL);
    const int withoutKeyWidth = MeasuredWidth(noKey, 1);
    Check(withKeyWidth > withoutKeyWidth,
          "同一份菜单换键位表后，Shell 菜单右端快捷键跟着变（自动显示）");
    DestroyMenu(popup2);
    noKey->Release();
    theme.Shutdown();

    std::printf(g_fail ? "\n有 %d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
