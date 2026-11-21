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
    // 深色/浅色的视图底与工具栏底也来自 UiPalette —— 色值单一来源，
    // 不在 Theme 里写第二份 RGB（2026-11-21 收编；此前这里硬编码且浅色
    // 取 GetSysColor(COLOR_WINDOW)=纯白，与注释实测的 #f3f3f3 不符）。
    bg = T.viewBg;
    bar = T.bar;
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

// ---------------------------------------------------------------- 标签颜色
//
// 色板是"分类标记"，不是设计工具：固定 7 档 + 1 个"无颜色"，不做取色器。
// （自由取色会让每个标签都是近似色，既认不出分组，深色底下还看不清 —— 与
//  VS Code / Chrome 标签组同一取舍：颜色只用来分区，不用来表达审美。）
//
// 两套色值（深/浅）而不是一套：同一份 RGB(0,120,215) 在浅色标签上过重、
// 在深色标签上过暗，压到"看得见但不抢文字"的强度必须按主题分别算。
namespace {

// 把 color 按 pct% 混进 base（pct=0 -> base，100 -> color）。
// 标签底色用它做低强度着色：文字色不变，靠色相区分，不靠明度对抗。
COLORREF Mix(COLORREF base, COLORREF color, int pct) {
    const int r = GetRValue(base) + (GetRValue(color) - GetRValue(base)) * pct / 100;
    const int g = GetGValue(base) + (GetGValue(color) - GetGValue(base)) * pct / 100;
    const int b = GetBValue(base) + (GetBValue(color) - GetBValue(base)) * pct / 100;
    return RGB(r, g, b);
}

}  // namespace

// 下标 0 = 无颜色；1..7 = 红/橙/黄/绿/蓝/紫/灰。
// 色值表已收进 ui_palette.h 的 TabColorsFor —— 这里不再有硬编码 RGB。

COLORREF Theme::TabColorFill(int idx) const {
    if (idx <= 0 || idx >= kTabColorCount) return CLR_INVALID;
    const COLORREF c = TabColorsFor(dark).colors[idx];
    // 填充只上 22%：再重就会与文字争对比度（实测深色 30% 时 sub 色标题已看不清）。
    return Mix(bar, c, dark ? 26 : 18);
}

COLORREF Theme::TabColorBar(int idx) const {
    if (idx <= 0 || idx >= kTabColorCount) return CLR_INVALID;
    return TabColorsFor(dark).colors[idx];
}

const wchar_t* Theme::TabColorName(int idx) {
    static const wchar_t* names[kTabColorCount] = {
        L"无颜色", L"红", L"橙", L"黄", L"绿", L"蓝", L"紫", L"灰",
    };
    if (idx < 0 || idx >= kTabColorCount) return names[0];
    return names[idx];
}
