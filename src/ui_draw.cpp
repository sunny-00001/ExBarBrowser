#include "ui_draw.h"

#include "icon_glyph.h"

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

}  // namespace

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
    HBRUSH b = CreateSolidBrush(color);
    FillRect(hdc, &rc, b);
    DeleteObject(b);
}

void FrameRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FrameRect(hdc, &rc, b);
    DeleteObject(b);
}

void FillRound(HDC hdc, const RECT& rc, COLORREF color, int radius) {
    // 半径比矩形还大时 RoundRect 会画出怪异形状，直接退化成直角填充。
    if (radius <= 0 || rc.right - rc.left < radius * 2 || rc.bottom - rc.top < radius * 2) {
        FillRectColor(hdc, rc, color);
        return;
    }
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ ob = SelectObject(hdc, b);
    HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(b);
}

void DrawSeparator(HDC hdc, const RECT& rc, bool vertical, COLORREF color, int thickness) {
    if (thickness < 1) thickness = 1;
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
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
    DeleteObject(pen);
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

// CurrentMods 已移到输入端（mouse_rules.h）：它读 GetKeyState，属输入状态。

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
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = bmp ? SelectObject(mem, bmp) : nullptr;
    if (bmp && oldBmp) {
        FillRectColor(mem, rc, bg);
        SetBkMode(mem, TRANSPARENT);
        if (draw) draw(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

}  // namespace ui
