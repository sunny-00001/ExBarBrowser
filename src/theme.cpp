#include "theme.h"

#include "config.h"       // ThemePrefersDark：深浅色档位的唯一判据来源
#include "log.h"          // ApplyProcessDarkMode 诊断
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


// ordinal 132: ShouldAppsUseDarkMode() —— 诊断用。
bool(WINAPI*ShouldAppsUseDarkMode())() {
    static auto fn = reinterpret_cast<bool(WINAPI*)()>(
        GetProcAddress(UxTheme(), MAKEINTRESOURCEA(132)));
    return fn;
}

// ordinal 104: RefreshImmersiveColorPolicyState() —— 让 uxtheme 重取当前进程模式。
// 实测 ForceDark 之后再改回浅色，只调 135 不够，新 EBC 仍按深色建。
void(WINAPI*RefreshImmersiveColorPolicyState())() {
    static auto fn = reinterpret_cast<void(WINAPI*)()>(
        GetProcAddress(UxTheme(), MAKEINTRESOURCEA(104)));
    return fn;
}

// ordinal 136: FlushMenuThemes()。菜单/控件主题缓存与颜色策略一起刷。
void(WINAPI*FlushMenuThemes())() {
    static auto fn = reinterpret_cast<void(WINAPI*)()>(
        GetProcAddress(UxTheme(), MAKEINTRESOURCEA(136)));
    return fn;
}

// 不在运行时广播 WM_SETTINGCHANGE("ImmersiveColorSet")：
// HWND_BROADCAST 会逐个等所有顶层窗口，实测每次 2.7~3.5s，是主题切换的
// 主要延迟。真正的修复是"先拆 EBC 再切模式最后重建"（见 app_layout.cpp），
// 旧 DirectUI 不存在时 shell 不需要这条通知；各控件也已有 ReloadTheme。

}  // namespace

void ApplyProcessDarkMode(bool dark) {
    // ordinal 135: SetPreferredAppMode(int) 0=Default 1=AllowDark 2=ForceDark 3=ForceLight
    //
    // 【切换顺序比模式值本身更关键】早期实现是"先切进程模式，再拆 EBC"：
    // 切到浅色时旧 DirectUI 深色窗口还活着，shell 会按它们的颜色策略创建
    // 新 EBC，于是文件区粘在深色（用户报的"只有双栏不变，必须重启"）。
    // 现在 App::ApplyTheme 先让所有 ShellView 拆掉浏览器，再调用这里，最后
    // 重建；因此这里只负责把进程模式明确压到目标档位。
    if (auto fn = PreferredMode()) {
        const int mode = dark ? 2 : 3;  // 2=ForceDark, 3=ForceLight
        const int prev = fn(mode);
        LogDbg(L"[theme] ApplyProcessDarkMode pid=%lu target=%d mode=%d prevMode=%d shouldNow=%d",
               GetCurrentProcessId(), dark ? 1 : 0, mode, prev,
               ShouldAppsUseDarkMode() ? 1 : 0);
    }
    if (auto refresh = RefreshImmersiveColorPolicyState()) refresh();
    if (auto flush = FlushMenuThemes()) flush();
}

void AllowDarkForWindow(HWND hwnd, bool dark) {
    if (!hwnd) return;
    if (auto fn = AllowDark()) fn(hwnd, dark ? TRUE : FALSE);
}

// 档位 -> 深浅色（见 theme.h：三处调用点共用的唯一判据）。
bool ModePrefersDark(const std::wstring& mode) {
    if (mode == thememode::kDark) return true;
    if (mode == thememode::kLight) return false;
    // system（含认不出的值）：跟随系统。认不出时不静默固定某一档 ——
    // 配置被手改坏时，"跟随系统"是最不可能让人以为程序坏了的行为。
    return SystemIsDark();
}

bool ThemePrefersDark() { return ModePrefersDark(Config::Get().s().themeMode); }

// 色值全部来自本机实测（ExBar 换栈阶段的教训：不能抄标准值）：
//   深色文件区 #141414、工具栏 #1e1e1e；浅色文件区 #ffffff、工具栏 #f3f3f3。
void Theme::Init(HWND hwnd, int dpiOverride, bool skipFonts) {
    dpi = dpiOverride > 0 ? dpiOverride : (hwnd ? DpiOf(hwnd) : 96);
    // 深浅色由配置档位决定（system / dark / light）：以前只有 SystemIsDark()，
    // 于是"晚上想用深色"必须把整个 Windows 一起调。
    dark = ThemePrefersDark();

    const UiPalette& T = PaletteFor(dark);
    // 深色/浅色的视图底与工具栏底也来自 UiPalette —— 色值单一来源，
    // 不在 Theme 里写第二份 RGB（2026-11-21 收编；此前这里硬编码且浅色
    // 取 GetSysColor(COLOR_WINDOW)=纯白，与注释实测的 #f3f3f3 不符）。
    bg = T.viewBg;
    // 所有自绘栏（标签/导航/书签/状态栏）的底 = Shell 文件列表的底 = viewBg。
    // 以前这里取 T.bar（浅 #f3f3f3 / 深 #1e1e1e），与文件列表不同色 —— 深浅两套
    // 下都能看到一条明显的色差分界。底不该有第二来源，直接复用 viewBg。
    // 标签激活态不靠底色区分：brBg 同为 viewBg，靠通栏 accent 色条 + text/sub 文字色差。
    bar = T.viewBg;
    // 其余令牌一律取 UiPalette，禁止自造
    barAlt = T.editBg;
    hover = T.hover;
    line = T.separator;
    navSep = T.navSep;   // 导航树分组线（explorer 实测 0x383838，与 separator 不同档）
    paneSep = T.paneSep; // 与 Shell 视图边框同色（AI 面板左边缘等"贴着视图的分界线"用）
    text = T.text;
    sub = T.dimText;
    inert = T.inert;
    accent = T.accent;
    pressed = T.pressed;
    disabled = T.disabled;
    chevron = T.chevron;
    editBg = T.editBg;
    editBorder = T.editBorder;
    listBg = T.listBg;
    listHot = T.listHotBg;
    listSel = T.listSelBg;
    selSolid = T.selSolidBg;
    selSolidFg = T.selSolidFg;
    panelBorder = T.panelBorder;
    // 设置对话框的四档底（说明面板 / 页签选中 / 当前行 / 行悬停）。
    panelBg = T.panelBg;
    softSel = T.softSel;
    wash = T.wash;
    rowHover = T.rowHover;
    danger = T.danger;

    brBg = CreateSolidBrush(bg);
    brBar = CreateSolidBrush(bar);
    brBarAlt = CreateSolidBrush(barAlt);
    brHover = CreateSolidBrush(hover);
    brPressed = CreateSolidBrush(pressed);
    brEdit = CreateSolidBrush(editBg);
    brList = CreateSolidBrush(listBg);
    brListHot = CreateSolidBrush(listHot);
    brListSel = CreateSolidBrush(listSel);
    brSelSolid = CreateSolidBrush(selSolid);
    brAccent = CreateSolidBrush(accent);
    // 视图区容器窗口类的背景刷：独立于 brBg，避免 GCLP_HBRBACKGROUND 指向
    // 一个随 Reload 被删的对象（曾经就是"切主题瞬间闪白框"的成因）。
    brViewBg = CreateSolidBrush(bg);

    // 字体只跟 DPI 与系统消息字体有关，与深/浅色无关。切主题时 DPI 没变，
    // 保留原句柄可避免每次 WM_SETFONT 触发 SHAutoComplete 重建（见 Reload）。
    if (skipFonts) return;

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
    // 等宽：Consolas 优先（Windows 自带），GDI 找不到时自动回落不影响排版。
    // 列表用与正文同号，输入区缓冲大一号 —— 与参考 mod 的 SeqFontSeq/SeqFontBuf 同口径。
    LOGFONTW lfMono = lf;
    lfMono.lfWeight = FW_NORMAL;
    lfMono.lfFaceName[0] = 0;
    lstrcpynW(lfMono.lfFaceName, L"Consolas", LF_FACESIZE);
    // 等宽用**像素**高度（13/14），不用磅值：磅值换算出来是 12px，
    // 而等宽字符宽要按 13px 算才与参考 mod 的列位一致（原版 SeqFontSeq = SX(13)）。
    lfMono.lfHeight = -MulDiv(13, dpi, 96);
    fontMono = CreateFontIndirectW(&lfMono);
    lfMono.lfHeight = -MulDiv(14, dpi, 96);
    fontMonoBig = CreateFontIndirectW(&lfMono);
    // 只有真正新建了字体才进代次。skipFonts 分支在前面已 return，切主题不会走到这里。
    ++fontGeneration;
}

void Theme::ApplyToViewArea(HWND area) {
    if (!area) return;
    SetClassLongPtrW(area, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brViewBg));
}

void Theme::Shutdown(bool keepFonts) {
    if (brBg) DeleteObject(brBg);
    if (brBar) DeleteObject(brBar);
    if (brBarAlt) DeleteObject(brBarAlt);
    if (brHover) DeleteObject(brHover);
    if (brPressed) DeleteObject(brPressed);
    if (brEdit) DeleteObject(brEdit);
    if (brList) DeleteObject(brList);
    if (brListHot) DeleteObject(brListHot);
    if (brListSel) DeleteObject(brListSel);
    if (brSelSolid) DeleteObject(brSelSolid);
    if (brAccent) DeleteObject(brAccent);
    if (brViewBg) DeleteObject(brViewBg);
    if (!keepFonts) {
        if (font) DeleteObject(font);
        if (fontBold) DeleteObject(fontBold);
        if (fontMono) DeleteObject(fontMono);
        if (fontMonoBig) DeleteObject(fontMonoBig);
    }
    brBg = brBar = brBarAlt = brHover = brPressed = brEdit = brList = brListHot = brListSel =
        brAccent = nullptr;
    brSelSolid = nullptr;
    brViewBg = nullptr;
    if (!keepFonts) font = fontBold = fontMono = fontMonoBig = nullptr;
}

void Theme::Reload(HWND hwnd, int dpiOverride) {
    // 字体只跟 DPI 和系统消息字体有关，与深/浅色无关。切主题（DPI 未变）时保留
    // 字体句柄：地址栏 EDIT 挂了 SHAutoComplete，每次收到新的 WM_SETFONT 都会
    // 重建自动完成列表，实测单控件 110~170ms，是切换主题最大的一块 UI 开销。
    // 只有 DPI 真的变了才连字体一起重建（否则绘制坐标会按旧 DPI 算）。
    const int prevDpi = dpi;
    const int newDpi = dpiOverride > 0 ? dpiOverride : (hwnd ? DpiOf(hwnd) : 96);
    const bool keepFonts = (newDpi == prevDpi && font != nullptr);
    Shutdown(/*keepFonts=*/keepFonts);
    // 传 newDpi 而不是原始 dpiOverride：Init 里不必再探测一次 DpiOf(hwnd)，
    // 避免两次探测之间窗口已移到不同 DPI 屏幕导致 keepFonts 判断与实际 dpi 不一致。
    Init(hwnd, newDpi, /*skipFonts=*/keepFonts);
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
