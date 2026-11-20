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

void DrawIconCentered(HDC hdc, HICON hIcon, RECT rc, int size) {
    if (!hIcon) return;
    DrawIconEx(hdc, rc.left + ((rc.right - rc.left) - size) / 2,
               rc.top + ((rc.bottom - rc.top) - size) / 2, hIcon, size, size, 0, nullptr,
               DI_NORMAL);
}

void DrawIconVCentered(HDC hdc, HICON hIcon, int x, int top, int bottom, int size) {
    if (!hIcon) return;
    DrawIconEx(hdc, x, top + ((bottom - top) - size) / 2, hIcon, size, size, 0, nullptr,
               DI_NORMAL);
}

void DrawIconFit(HDC hdc, HICON hIcon, const RECT& slot, int slotPx) {
    if (!hIcon) return;
    // 按图标真实像素画：16px 图标塞进 20px 槽位时不要拉伸（SHGFI 给的就是既有位图）
    const int real = IconPx(hIcon, slotPx);
    const int cy = slot.top + ((slot.bottom - slot.top) - real) / 2;
    const int cx = slot.left + (slotPx - real) / 2;
    DrawIconEx(hdc, cx, cy, hIcon, real, real, 0, nullptr, DI_NORMAL);
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
