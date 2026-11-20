#include "theme.h"

#include "ui_palette.h"

namespace {

HMODULE UxTheme() {
    static HMODULE m = LoadLibraryW(L"uxtheme.dll");
    return m;
}

// ordinal 135: SetPreferredAppMode(int) 0=Default 1=AllowDark 2=ForceDark 3=ForceLight
int(WINAPI*PreferredMode())(int) {
    static auto fn = reinterpret_cast<int(WINAPI*)(int)>(
        GetProcAddress(UxTheme(), MAKEINTRESOURCEA(135)));
    return fn;
}

// ordinal 133: AllowDarkModeForWindow(HWND, BOOL)
BOOL(WINAPI*AllowDark())(HWND, BOOL) {
    static auto fn = reinterpret_cast<BOOL(WINAPI*)(HWND, BOOL)>(
        GetProcAddress(UxTheme(), MAKEINTRESOURCEA(133)));
    return fn;
}

}  // namespace

void ApplyProcessDarkMode(bool dark) {
    if (auto fn = PreferredMode()) fn(dark ? 2 : 3);
}

void AllowDarkForWindow(HWND hwnd, bool dark) {
    if (!hwnd) return;
    if (auto fn = AllowDark()) fn(hwnd, dark ? TRUE : FALSE);
}

// 色值全部来自本机实测（ExBar 换栈阶段的教训：不能抄标准值）：
//   深色文件区 #141414、工具栏 #1e1e1e；浅色文件区 #ffffff、工具栏 #f3f3f3。
void Theme::Init(HWND hwnd, int dpiOverride) {
    dpi = dpiOverride > 0 ? dpiOverride : (hwnd ? DpiOf(hwnd) : 96);
    dark = SystemIsDark();

    const UiPalette& T = PaletteFor(dark);
    if (dark) {
        bg = RGB(0x14, 0x14, 0x14);              // 视图容器底（shell 自己会画）
        bar = RGB(32, 32, 32);                    // 工具栏底
    } else {
        bg = RGB(0xff, 0xff, 0xff);
        bar = GetSysColor(COLOR_WINDOW);          // 工具栏底 = 系统窗口色
    }
    // 其余令牌一律取 UiPalette，禁止自造
    barAlt = T.editBg;
    hover = T.hover;
    line = T.separator;
    text = T.text;
    sub = T.dimText;
    accent = T.accent;
    pressed = T.pressed;
    disabled = T.disabled;
    chevron = T.chevron;
    editBg = T.editBg;
    editBorder = T.editBorder;
    listBg = T.listBg;
    listHot = T.listHotBg;
    listSel = T.listSelBg;

    brBg = CreateSolidBrush(bg);
    brBar = CreateSolidBrush(bar);
    brBarAlt = CreateSolidBrush(barAlt);
    brHover = CreateSolidBrush(hover);
    brPressed = CreateSolidBrush(pressed);
    brEdit = CreateSolidBrush(editBg);
    brList = CreateSolidBrush(listBg);
    brListHot = CreateSolidBrush(listHot);
    brListSel = CreateSolidBrush(listSel);
    brAccent = CreateSolidBrush(accent);
    // 视图区容器窗口类的背景刷：独立于 brBg，避免 GCLP_HBRBACKGROUND 指向
    // 一个随 Reload 被删的对象（曾经就是"切主题瞬间闪白框"的成因）。
    brViewBg = CreateSolidBrush(bg);

    // 正文/加粗：系统 message font（中文 = Microsoft YaHei UI），与 mod 同源。
    // 字形不再自持 HFONT —— 一律走 ui::DrawGlyph 的字体池。
    LOGFONTW lf{};
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(9, dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (lf.lfFaceName[0] == 0) lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    fontBold = CreateFontIndirectW(&lf);
}

void Theme::ApplyToViewArea(HWND area) {
    if (!area) return;
    SetClassLongPtrW(area, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brViewBg));
}

void Theme::Shutdown() {
    if (brBg) DeleteObject(brBg);
    if (brBar) DeleteObject(brBar);
    if (brBarAlt) DeleteObject(brBarAlt);
    if (brHover) DeleteObject(brHover);
    if (brPressed) DeleteObject(brPressed);
    if (brEdit) DeleteObject(brEdit);
    if (brList) DeleteObject(brList);
    if (brListHot) DeleteObject(brListHot);
    if (brListSel) DeleteObject(brListSel);
    if (brAccent) DeleteObject(brAccent);
    if (brViewBg) DeleteObject(brViewBg);
    if (font) DeleteObject(font);
    if (fontBold) DeleteObject(fontBold);
    brBg = brBar = brBarAlt = brHover = brPressed = brEdit = brList = brListHot = brListSel =
        brAccent = nullptr;
    brViewBg = nullptr;
    font = fontBold = nullptr;
}

void Theme::Reload(HWND hwnd, int dpiOverride) {
    Shutdown();
    Init(hwnd, dpiOverride);
}
