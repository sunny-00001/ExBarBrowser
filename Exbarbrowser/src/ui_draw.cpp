#include "ui_draw.h"

#include "icon_glyph.h"

#include <algorithm>
#include <cmath>    // std::pow —— 相对亮度要还原 gamma
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ui {
namespace {

struct UiFontEntry {
    std::wstring key;
    HFONT font = nullptr;
};

std::vector<UiFontEntry>& FontPool() {
    static std::vector<UiFontEntry> pool;
    return pool;
}

std::unordered_map<COLORREF, HBRUSH>& BrushCache() {
    static std::unordered_map<COLORREF, HBRUSH> cache;
    return cache;
}

// 高 32 位放 COLORREF，低 32 位放线宽；线宽为正整数。
std::uint64_t PenKey(COLORREF color, int thickness) {
    return (static_cast<std::uint64_t>(color) << 32) |
           static_cast<std::uint32_t>(thickness);
}

std::unordered_map<std::uint64_t, HPEN>& PenCache() {
    static std::unordered_map<std::uint64_t, HPEN> cache;
    return cache;
}

HBRUSH CachedBrush(COLORREF color) {
    auto& cache = BrushCache();
    if (const auto it = cache.find(color); it != cache.end()) return it->second;
    HBRUSH b = CreateSolidBrush(color);
    if (b) cache.emplace(color, b);
    return b;
}

HPEN CachedPen(COLORREF color, int thickness) {
    const std::uint64_t key = PenKey(color, thickness);
    auto& cache = PenCache();
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    HPEN p = CreatePen(PS_SOLID, thickness, color);
    if (p) cache.emplace(key, p);
    return p;
}

}  // namespace

void ClearGdiCaches() {
    for (auto& e : FontPool())
        if (e.font) DeleteObject(e.font);
    FontPool().clear();
    for (auto& kv : BrushCache())
        if (kv.second) DeleteObject(kv.second);
    BrushCache().clear();
    for (auto& kv : PenCache())
        if (kv.second) DeleteObject(kv.second);
    PenCache().clear();
}

// FontPx / Scale / IconSize 已删（零调用，且在造"第二个 DPI 换算公式"）。
// DPI 缩放唯一入口是 Theme::S()。见 ui_draw.h 的注释与 docs/NAMING.md §3.2。

namespace detail {

HFONT GetUiFont(int heightPx, int weight, const wchar_t* face) {
    if (heightPx < 8) heightPx = 8;
    std::wstring key = face;
    key += L'|';
    key += std::to_wstring(heightPx);
    key += L'|';
    key += std::to_wstring(weight);
    for (const auto& e : FontPool())
        if (e.key == key) return e.font;

    const bool glyphFace = !wcscmp(face, L"Segoe Fluent Icons") || !wcscmp(face, L"Segoe MDL2 Assets");
    HFONT f = CreateFontW(-heightPx, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          glyphFace ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, face);
    if (f) FontPool().push_back({std::move(key), f});
    return f;
}

const wchar_t* GlyphFace() {
    static int probe = -1;
    if (probe < 0) {
        HDC dc = GetDC(nullptr);
        HFONT f = GetUiFont(12, FW_NORMAL, L"Segoe Fluent Icons");
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t face[64] = L"";
        GetTextFaceW(dc, 64, face);
        probe = (wcscmp(face, L"Segoe Fluent Icons") == 0) ? 1 : 0;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    return probe ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

}  // namespace detail

void DrawGlyph(HDC hdc, wchar_t glyph, RECT rc, int px, COLORREF clr, UINT align) {
    if (px < 8) px = 8;
    int saved = SaveDC(hdc);
    HFONT f = detail::GetUiFont(px, FW_NORMAL, detail::GlyphFace());
    HFONT of = static_cast<HFONT>(SelectObject(hdc, f));
    SetTextColor(hdc, clr);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, &glyph, 1, &rc, align | DT_SINGLELINE);
    if (of) SelectObject(hdc, of);
    if (saved != 0) RestoreDC(hdc, saved);
}

void DrawChevron(HDC hdc, int cx, int cy, int px, COLORREF clr, int dir) {
    // 直接查字形表（ico::Glyph），不经任何名字层
    const ico::IconId id = dir < 0 ? ico::IconId::ChevronUp
                                   : (dir == 0 ? ico::IconId::ChevronLeft
                                               : ico::IconId::ChevronDown);
    wchar_t g = ico::Glyph(id);
    RECT r{cx - px, cy - px, cx + px, cy + px};
    DrawGlyph(hdc, g, r, px, clr, DT_CENTER | DT_VCENTER);
}

HICON StockIcon(int siid, bool small) {
    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(siid),
                                     SHGSI_ICON | (small ? SHGSI_SMALLICON : SHGSI_LARGEICON), &sii)) &&
        sii.hIcon)
        return sii.hIcon;
    return nullptr;
}

// 图标的落笔尺寸：真实像素与槽宽取小者。
// 理由见 ui_draw.h 的注释：ico::Resolve 给的是位图，放大只会糊；
// 只有"槽比图标还小"（真的放不下）时才缩，且缩到正好塞进槽。
static int IconDrawPx(HICON hIcon, int slotPx) {
    const int real = IconPx(hIcon, slotPx);
    if (slotPx <= 0) return real;
    return real <= slotPx ? real : slotPx;
}

void DrawIconCentered(HDC hdc, HICON hIcon, const RECT& rc, int slotPx) {
    if (!hIcon) return;
    const int px = IconDrawPx(hIcon, slotPx);
    DrawIconEx(hdc, rc.left + ((rc.right - rc.left) - px) / 2,
               rc.top + ((rc.bottom - rc.top) - px) / 2, hIcon, px, px, 0, nullptr, DI_NORMAL);
}

void DrawIconVCentered(HDC hdc, HICON hIcon, int x, int top, int bottom, int slotPx) {
    if (!hIcon) return;
    const int px = IconDrawPx(hIcon, slotPx);
    DrawIconEx(hdc, x, top + ((bottom - top) - px) / 2, hIcon, px, px, 0, nullptr, DI_NORMAL);
}

void FillRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    if (HBRUSH b = CachedBrush(color)) FillRect(hdc, &rc, b);
}

void FrameRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    if (HBRUSH b = CachedBrush(color)) FrameRect(hdc, &rc, b);
}

void FillRound(HDC hdc, const RECT& rc, COLORREF color, int radius) {
    // 半径比矩形还大时 RoundRect 会画出怪异形状，直接退化成直角填充。
    if (radius <= 0 || rc.right - rc.left < radius * 2 || rc.bottom - rc.top < radius * 2) {
        FillRectColor(hdc, rc, color);
        return;
    }
    if (HBRUSH b = CachedBrush(color)) {
        HGDIOBJ ob = SelectObject(hdc, b);
        HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
    }
}

void DrawSeparator(HDC hdc, const RECT& rc, bool vertical, COLORREF color, int thickness) {
    if (thickness < 1) thickness = 1;
    HPEN pen = CachedPen(color, thickness);
    if (!pen) return;
    HGDIOBJ old = SelectObject(hdc, pen);
    if (vertical) {
        const int x = rc.left;
        MoveToEx(hdc, x, rc.top, nullptr);
        LineTo(hdc, x, rc.bottom);
    } else {
        const int y = rc.top;
        MoveToEx(hdc, rc.left, y, nullptr);
        LineTo(hdc, rc.right, y);
    }
    SelectObject(hdc, old);
}

int MeasureText(HDC hdc, HFONT font, const wchar_t* text, int len) {
    if (!hdc || !text || !*text) return 0;
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    SIZE sz{};
    const BOOL ok = GetTextExtentPoint32W(hdc, text, len < 0 ? static_cast<int>(wcslen(text)) : len,
                                         &sz);
    if (old) SelectObject(hdc, old);
    return ok ? static_cast<int>(sz.cx) : 0;
}

int MeasureStableText(HDC hdc, HFONT font, const std::wstring& text) {
    if (!hdc || text.empty()) return 0;
    // 数字换 '8'：字体里数字多为等宽，'8' 是其中的代表宽度。
    // 换完再走 MeasureText —— 量字只有这一份实现（GetTextExtentPoint32W 只允许在
    // ui_draw.cpp 出现，见 tools/check_components.py 的"文本测量"规则）。
    std::wstring m = text;
    for (wchar_t& c : m)
        if (c >= L'0' && c <= L'9') c = L'8';
    return MeasureText(hdc, font, m);
}

int EditLineHeight(HDC hdc, HFONT font, int fallbackPx) {
    // 实测（EM_POSFROMCHAR）：原生单行 EDIT 的文字顶部恒为 0，不随控件窗口高度居中。
    // 所以控件窗口高度必须≈文字实际高度，文字才会落在我们指定的那一行；窗口越高，
    // 文字越贴顶。再偶数化，才能在偶数行高里做到上下留白严格对称。
    const auto even = [](int h) { return (h & 1) ? h + 1 : h; };
    if (!hdc) return even(fallbackPx);
    ScopedFont scoped(hdc, font);
    TEXTMETRICW tm{};
    if (!GetTextMetricsW(hdc, &tm) || tm.tmHeight <= 0) return even(fallbackPx);
    return even(static_cast<int>(tm.tmHeight));
}

void DrawTextRow(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color,
                 UINT fmt) {
    if (!hdc) return;
    // 选字体（可能为空：调用方只想要默认字体色），并在返回前**无条件**还原。
    // 不还原的后果不是"这一行画错"，而是同一次 WM_PAINT 里后续所有绘制都串体 ——
    // 这类 bug 看起来像"随机的字体错乱"，很难定位到具体那一行。
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    SetTextColor(hdc, color);
    RECT r = rc;   // DrawTextW 会改 rc（DT_CALCRECT/居中时会写回），不能传调用方的
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, fmt);
    if (old) SelectObject(hdc, old);
}

int MeasureTextHeight(HDC hdc, HFONT font, const std::wstring& text, const RECT& box, UINT fmt) {
    if (!hdc || text.empty()) return 0;
    // 和 DrawTextRow 同一套"选字体 → 还原"纪律：DT_CALCRECT 也用 hdc 当前字体，
    // 不还原会让后续绘制串体（见 DrawTextRow 的说明）。
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    RECT r = box;   // DT_CALCRECT 会写回 r，不能传调用方的 box
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r,
              fmt | DT_CALCRECT);
    if (old) SelectObject(hdc, old);
    return r.bottom - r.top;
}

// CurrentMods 已移到输入端（mouse_rules.h）：它读 GetKeyState，属输入状态。

// ---------------------------------------------------------------------------
// 颜色对比度（见 ui_draw.h 的说明：前景/背景不是一对时必须按实际对比度兜底）
// ---------------------------------------------------------------------------

namespace {

// sRGB 分量 -> 线性。gamma 2.2 的近似（WCAG 用分段公式，这里按同一分段做，
// 差别在小数点后第 4 位，对"够不够读"的判定没有影响）。
double SrgbToLinear(unsigned char v) {
    const double s = v / 255.0;
    return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

}  // namespace

double RelativeLuminance(COLORREF c) {
    // COLORREF 是 0x00BBGGRR（RGB 宏的字节顺序）。
    const unsigned char r = GetRValue(c);
    const unsigned char g = GetGValue(c);
    const unsigned char b = GetBValue(c);
    return 0.2126 * SrgbToLinear(r) + 0.7152 * SrgbToLinear(g) + 0.0722 * SrgbToLinear(b);
}

double ContrastRatio(COLORREF a, COLORREF b) {
    const double la = RelativeLuminance(a);
    const double lb = RelativeLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

COLORREF EnsureContrast(COLORREF fg, COLORREF bg, double minRatio) {
    if (ContrastRatio(fg, bg) >= minRatio) return fg;   // 主题配得好，原样用

    // 不足就二选一：黑或白里对比度更高的那个。
    // 不选"把 fg 提亮/压暗一档"是因为那需要反复迭代，且结果依赖初始色；
    // 黑白只有一个答案，且必然可达 21:1 上限内的高值。
    const COLORREF black = RGB(0, 0, 0);
    const COLORREF white = RGB(255, 255, 255);
    return ContrastRatio(black, bg) >= ContrastRatio(white, bg) ? black : white;
}

COLORREF MixColor(COLORREF fg, COLORREF bg, int t) {
    if (t < 0) t = 0;
    if (t > 100) t = 100;
    const int r = (GetRValue(fg) * t + GetRValue(bg) * (100 - t)) / 100;
    const int g = (GetGValue(fg) * t + GetGValue(bg) * (100 - t)) / 100;
    const int b = (GetBValue(fg) * t + GetBValue(bg) * (100 - t)) / 100;
    return RGB(r, g, b);
}

void PaintBuffered(HWND hwnd, COLORREF bg, const std::function<void(HDC, const RECT&)>& draw) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC mem = CreateCompatibleDC(dc);
    if (!mem) {
        EndPaint(hwnd, &ps);
        return;
    }
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    if (!bmp) {
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    if (!oldBmp || oldBmp == HGDI_ERROR) {
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return;
    }

    FillRectColor(mem, rc, bg);
    SetBkMode(mem, TRANSPARENT);
    if (draw) draw(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

}  // namespace ui
