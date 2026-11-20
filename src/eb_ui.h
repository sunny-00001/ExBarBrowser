// ExBar UI 原语（从 001-eb.cpp / 002-ebda.cpp 搬运）：
//   字体池 GetUiFont / 字号刻度 FontPx / 字形面 GlyphFace / 图标登记 IconGlyph /
//   DrawGlyph / DrawChevron / StockIcon / DrawIconCentered / DrawIconVCentered /
//   Scale / IconSize / FillRectColor / FrameRectColor
//
// 目的：栏、面包屑、下拉全部走同一套原语，不再各写一份（避免"两套观感"）。
//
// 【唯一性纪律】以下原语各只允许一份实现，任何栏/弹层/对话框都只能调用，不得自写：
//   · 填充 / 描边 / 分隔线 / 圆角底板 —— 以前 7 处各自 CreatePen+MoveToEx+LineTo
//   · 量字 —— 以前 6 处各自 GetDC+SelectObject+GetTextExtentPoint32W
//   · 图标居中 —— 以前 4 处各自 IconPx+DrawIconEx
//   · 修饰键 —— 以前 4 个文件各一份 CurrentMods
// 判据见 tools/check_components.py 的 REGISTRY。
#pragma once
#include "common.h"
#include "icon_glyph.h"

namespace eb {

// 字号角色（与 mod 的 FontRole 同值）
enum FontRole {
    FR_UI = 12,
    FR_UI_SMALL = 11,
    FR_GLYPH = 12,
    FR_GLYPH_LG = 14,
    FR_GLYPH_SMALL = 10,
};

int FontPx(FontRole role, UINT dpi);
HFONT GetUiFont(int heightPx, int weight, const wchar_t* face);
const wchar_t* GlyphFace();
wchar_t IconGlyph(const wchar_t* name);

void DrawGlyph(HDC hdc, wchar_t glyph, RECT rc, int px, COLORREF clr,
               UINT align = DT_CENTER | DT_VCENTER);
// 推荐入口：不写码位。语义全在 ico::IconId 里，改字形只动 icon_glyph.cpp。
inline void DrawGlyph(HDC hdc, ico::IconId id, RECT rc, int px, COLORREF clr,
                      UINT align = DT_CENTER | DT_VCENTER) {
    DrawGlyph(hdc, ico::Glyph(id), rc, px, clr, align);
}
void DrawChevron(HDC hdc, int cx, int cy, int px, COLORREF clr, int dir = 1);

HICON StockIcon(int siid, bool small);
void DrawIconCentered(HDC hdc, HICON hIcon, RECT rc, int size);
void DrawIconVCentered(HDC hdc, HICON hIcon, int x, int top, int bottom, int size);
// 在 slot 见方的槽里居中画图标，但**按图标自身真实像素**画（SHGFI_SMALLICON 给的
// 可能小于槽位，强行拉伸会发虚）。以前 4 处各写一遍 IconPx + 手算居中偏移。
void DrawIconFit(HDC hdc, HICON hIcon, const RECT& slot, int slotPx);

int Scale(int v96, UINT dpi);
int IconSize(bool small, UINT dpi);

void FillRectColor(HDC hdc, const RECT& rc, COLORREF color);
void FrameRectColor(HDC hdc, const RECT& rc, COLORREF color);

// 圆角填充（悬停/按下底板）。以前只存在于 nav_bar_draw.cpp 的匿名命名空间 ——
// 别的栏想用拿不到，于是各自退回直角 FillRect，这就是"导航栏圆角、其余栏直角"
// 这套两套观感的真实成因。半径由调用方给（一般用 Theme::radius_btn）。
void FillRound(HDC hdc, const RECT& rc, COLORREF color, int radius);

// 分隔线（1px 横/竖）。以前 7 处各自 CreatePen + MoveToEx + LineTo + DeleteObject。
// vertical=true 时用 rc.left 作 x 画竖线；否则用 rc.top 作 y 画横线。
void DrawSeparator(HDC hdc, const RECT& rc, bool vertical, COLORREF color, int thickness = 1);

// 量字：选字体 → GetTextExtentPoint32W → 还原原字体。
// 以前 6 个文件各写一遍同样的 8 行，且各自忘记还原导致后续绘制串体。
// len < 0 表示按 NUL 结尾计算。
int MeasureText(HDC hdc, HFONT font, const wchar_t* text, int len = -1);
inline int MeasureText(HDC hdc, HFONT font, const std::wstring& text) {
    return MeasureText(hdc, font, text.c_str(), static_cast<int>(text.size()));
}

// 修饰键位（1 Ctrl / 2 Shift / 4 Alt），与 data/mouse.json 的 mods 同编码。
// 以前 nav_bar / status_bar / bookmark_bar / tab_strip_input 各有一份 CurrentMods。
int CurrentMods();

// 双缓冲绘制：BeginPaint → 离屏 DC/位图 → 背景色 → draw() → BitBlt → 清理 → EndPaint。
// 以前 5 个文件各写一遍同样的 20 行样板，且都要自己记得 WM_ERASEBKGND return 1。
// draw 拿到的 HDC 已设好 TRANSPARENT bkmode 与背景填充，只需画内容。
void PaintBuffered(HWND hwnd, COLORREF bg, const std::function<void(HDC, const RECT&)>& draw);

}  // namespace eb
