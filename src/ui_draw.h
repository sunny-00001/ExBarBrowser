// UI 绘制原语（唯一实现）。
//
// 【名字由来】本文件原叫 `eb_ui.*`、命名空间 `eb`。`eb` = Ex**B**ar，是独立前那个
// Windhawk mod 的缩写 —— 相当于"用上一个项目的前缀写自己的代码"。Qt 社区对 `Q`
// 前缀的结论同样适用于此：**框架前缀只属于框架，自己的代码用命名空间 + 领域名**。
// 工程 2026-11-20 独立后已改为 `ui` 命名空间 + 领域文件名（见 docs/NAMING.md §3.2）。
//
// 【唯一性纪律】以下原语各只允许一份实现，任何栏/弹层/对话框都只能调用，不得自写：
//   · 填充 / 描边 / 分隔线 / 圆角底板 —— 以前 7 处各自 CreatePen+MoveToEx+LineTo
//   · 量字 —— 以前 8 处各自 GetDC+SelectObject+GetTextExtentPoint32W
//   · 图标居中 —— 以前 4 处各自 IconPx+DrawIconEx
//   · 双缓冲 —— 以前 5 个文件各写一遍同样的 20 行
// 判据见 tools/check_components.py。
//
// 【本层不含】修饰键（读 GetKeyState，属输入端 → mouse_rules.h）；
// 色值令牌（→ ui_palette.h / theme.h）；字形码位表（→ icon_glyph.h）。
#pragma once
#include "common.h"
#include "icon_glyph.h"

namespace ui {

// ---- 字体与字形 ----
// 字体池与字形面探测**不对外**：调用方一律用 DrawGlyph / DrawChevron，
// 由它们内部选字体。以前这两个是公开声明，实际上零外部调用（校验器的
// "应私有化"提示抓到的就是它）—— 公开等于给了"绕过 DrawGlyph 自己建字体"的口子，
// 而字体一旦分叉就会出现"两套观感"。
namespace detail {
HFONT GetUiFont(int heightPx, int weight, const wchar_t* face);
const wchar_t* GlyphFace();   // 优先 Segoe Fluent Icons（Win11），回落 MDL2（Win10）
}  // namespace detail

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

// 注：这里曾有 FontPx / Scale / IconSize 三个"DPI 换算"入口，全部零调用 ——
// 真实缩放一律走 Theme::S()（它对 dpi 的默认值处理在同一处）。三个入口已于
// 2026-11-20 删除：一个概念两个公式是"两套观感"的成因，不是解法。
// 见 docs/NAMING.md §3.2。

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

// 注：CurrentMods 已移入输入端（mouse_rules.h）—— 它读 GetKeyState，是输入状态而非
// 绘制资源。放在 UI 原语里会让"UI 反问输入状态"，与 R15 的分层方向相反。

// 双缓冲绘制：BeginPaint → 离屏 DC/位图 → 背景色 → draw() → BitBlt → 清理 → EndPaint。
// 以前 5 个文件各写一遍同样的 20 行样板，且都要自己记得 WM_ERASEBKGND return 1。
// draw 拿到的 HDC 已设好 TRANSPARENT bkmode 与背景填充，只需画内容。
void PaintBuffered(HWND hwnd, COLORREF bg, const std::function<void(HDC, const RECT&)>& draw);

}  // namespace ui
