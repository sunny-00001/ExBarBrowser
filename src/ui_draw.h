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
// ---- 图标绘制：落笔尺寸 = 图标真实像素，槽只决定位置与上限 ----
// ico::Resolve 返回的是系统给的既有位图（SM_CXSMICON 像素，随 DPI 变 16/20/24…），
// 不是矢量。以前这里有**三个**原语、**两套**缩放规则：
//    DrawIconCentered / DrawIconVCentered -> 按调用方给的 size 拉伸
//    DrawIconFit(已删)                    -> 按 IconPx(icon) 真实像素
// 后果不是"糊"，而是**同一枚图标画出来的尺寸不同**：在 120 DPI 下源图标 20px，
// 导航栏 14dp 槽（18px）走 Centered 被压成 18px，弹层 16dp 槽（20px）走 Fit 保持 20px。
// 用户看到的是"两个地方的图标不一样大"，而代码里两处读的都是同一个常量。
// （实测补充：DrawIconEx 缩放图标**不做插值** —— 20px 硬边图标画成 18px 时中间 alpha
//   为 0 个，所以"发虚"这个理由站不住；真正的问题是尺寸不一致，别再用糊边当论据。）
//
// 现在只剩一套规则：槽宽只是**上限**，不放大；槽比图标小（真的放不下）时才缩到槽宽。
// 于是"传进去的槽"和"画出来多大"的关系是确定的，不再取决于调用的是哪个函数。
void DrawIconCentered(HDC hdc, HICON hIcon, const RECT& rc, int slotPx);
// 左对齐版（前导图标：x 固定，垂直居中）。同一套规则。
void DrawIconVCentered(HDC hdc, HICON hIcon, int x, int top, int bottom, int slotPx);

// 注：这里曾有 FontPx / Scale / IconSize 三个"DPI 换算"入口，全部零调用 ——
// 真实缩放一律走 Theme::S()（它对 dpi 的默认值处理在同一处）。三个入口已于
// 2026-11-20 删除：一个概念两个公式是"两套观感"的成因，不是解法。
// 见 docs/NAMING.md §3.2。

void FillRectColor(HDC hdc, const RECT& rc, COLORREF color);
void FrameRectColor(HDC hdc, const RECT& rc, COLORREF color);
// 进程退出时统一释放绘制层持有的 GDI 句柄（字体池 + 实色画刷/画笔缓存）。
// 不在此期间仍有窗口在绘制；主流程只在 App::Run 返回后调用。
void ClearGdiCaches();

// ---- 颜色对比度工具（绘图层唯一实现，不要在调用方另写一份）----
//
// 【为什么需要】"前景色由主题给、背景色也由主题给"听起来成对，实际不是：
// Theme::text 是"内容区背景上的文字色"，而占位页/说明面板用的是 panelBg，
// 两者从未被配成一对。实测（2026-11-23）浅色主题下 AI 面板占位页画出来是
// 白底白字 —— 用户看到的是一块空白，而代码里"明明设了颜色"。
// 这类问题的正确解法不是"去找主题要另一组值"，而是按**实际对比度**兜底：
// 只要对比度不足就换成黑或白，永远有可读的文字。

// WCAG 相对亮度（0..1）。sRGB 分量先做 gamma 还原再按 0.2126/0.7152/0.0722 加权。
double RelativeLuminance(COLORREF c);

// WCAG 对比度：(L1+0.05)/(L2+0.05)，1..21。1 = 同色。
double ContrastRatio(COLORREF a, COLORREF b);

// 确保 fg 在 bg 上可读：当前对比度低于 minRatio（默认 4.5，WCAG AA 正文）
// 就改用纯黑或纯白中对比度更高的那个。达标时**原样返回 fg** ——
// 不因为"可能不够好"就一律覆盖主题配色。
COLORREF EnsureContrast(COLORREF fg, COLORREF bg, double minRatio = 4.5);

// 两色线性混合，t 是 fg 的权重（0..100）。用于把次要信息（诊断行）弱化，
// 同时保证它仍落在 bg 与 fg 之间，不会突然跑出可读范围。
COLORREF MixColor(COLORREF fg, COLORREF bg, int t);

// 圆角填充（悬停/按下底板）。以前只存在于 nav_bar_draw.cpp 的匿名命名空间 ——
// 别的栏想用拿不到，于是各自退回直角 FillRect，这就是"导航栏圆角、其余栏直角"
// 这套两套观感的真实成因。半径由调用方给（一般用 Theme::radius_btn）。
void FillRound(HDC hdc, const RECT& rc, COLORREF color, int radius);

// 分隔线（1px 横/竖）。以前 7 处各自 CreatePen + MoveToEx + LineTo + DeleteObject。
// vertical=true 时用 rc.left 作 x 画竖线；否则用 rc.top 作 y 画横线。
void DrawSeparator(HDC hdc, const RECT& rc, bool vertical, COLORREF color, int thickness = 1);

// 单行 EDIT 的统一垂直度量（导航栏地址框/搜索框先落地，设置对话框复用）。
//
// 【实测原理，不是猜像素】EM_POSFROMCHAR 量到：原生单行 EDIT 的文字顶部恒为 0，
// 不随控件窗口高度居中（H=17/25/40 时文字顶部都为 0）。所以控件窗口高度必须≈
// 文字实际高度，文字才会落在指定那一行；窗口越高，文字越贴顶、底部越空。
// 这里返回 >= tmHeight 的最小偶数：偶数高度既让 EDIT 文字（贴顶）与自绘
// DT_VCENTER 文字（在偶数矩形里 top=(H-字高)/2=0）落在同一像素，又能在偶数
// 行高（40/28/24）里做到上下留白严格对称。两处各写一遍迟早分叉。
constexpr int kEditVPad96 = 1;  // 边框内、文字上下的垂直留白（各 1px）

// 行高按给定字体的真实度量计算（偶数化）；取不到度量时用调用方给的 DPI 缩放兜底值。
int EditLineHeight(HDC hdc, HFONT font, int fallbackPx);

// 把 lineH 高的文字行在任意框里垂直居中；框不够高时退化为框本身。
// 这是绘制与子控件定位共用的唯一换算，避免“画在一行、EDIT 在另一行”。
inline RECT CenteredLineRect(const RECT& box, int lineH) {
    const int boxH = box.bottom - box.top;
    const int h = lineH > boxH ? boxH : lineH;
    const int top = box.top + (boxH - h) / 2;
    return {box.left, top, box.right, top + h};
}

// 选字体 → 绘制 → 还原的 RAII 收编。
//
// 【为什么需要】以前 5 个自绘栏（书签/标签/序列提示/序列弹层/导航）各写一遍：
//     HGDIOBJ old = SelectObject(dc, theme_->font);
//     ... 画 ...
//     SelectObject(dc, old);
// 中间任意早退（return / 异常）漏还原，后续绘制就串体。把它收成一个类，
// 由作用域保证还原，且调用方不再需要记住"old"这个变量。
// 单行文本仍优先 DrawTextRow（它内部已自带选/还原）；只有"整段共用一个字体"才用本类。
class ScopedFont {
public:
    ScopedFont(HDC hdc, HFONT font)
        : hdc_(hdc), old_(font ? SelectObject(hdc, font) : nullptr) {}
    ~ScopedFont() {
        if (old_ && old_ != HGDI_ERROR) SelectObject(hdc_, old_);
    }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;

private:
    HDC hdc_;
    HGDIOBJ old_;
};

// 量字：选字体 → GetTextExtentPoint32W → 还原原字体。
// 以前 6 个文件各写一遍同样的 8 行，且各自忘记还原导致后续绘制串体。
// len < 0 表示按 NUL 结尾计算。
int MeasureText(HDC hdc, HFONT font, const wchar_t* text, int len = -1);

// 数值段的"稳定宽"：把数字全换成 '8' 再量 —— 宽度只跟**位数**走，与具体数值无关。
//
// 【为什么不用"历史最大宽"】那种口径只会变宽不会变窄：进过一次大目录后该段永久
// 偏宽，窄窗口下白占空间，还要把别的段挤掉。位数控宽是纯函数 —— 同样的数字形状
// 永远同样宽，"5"变"7"不抖，只有位数变了才变。
//
// 【口径来源】Windhawk Exbar.cpp 的 StableTextW（同一思路）：状态栏的计数/容量段
// 用它预留宽度，数值变化时整条栏不再左右抖。
int MeasureStableText(HDC hdc, HFONT font, const std::wstring& text);

// 画一行文本（选字体 + 设颜色 + DrawTextW + 还原的合并版）。
//
// 【为什么需要它】MeasureText 只收了"量"，没收"画"。结果 7 个文件里 11 处
// 各写一遍这个四步序列：
//     HGDIOBJ old = SelectObject(dc, font);
//     SetTextColor(dc, color);
//     DrawTextW(dc, text, -1, &rc, fmt);
//     SelectObject(dc, old);
// 每一处都是一个"忘记还原就串体"的机会，且字体必须由调用方自己知道该用哪个。
//
// fmt 由调用方给（DT_END_ELLIPSIS / DT_PATH_ELLIPSIS / DT_CENTER 等差异是真实需求，
// 不该被这个函数吃掉）；这里只统一"选字体-设色-还原"这三步与字符串长度处理。
void DrawTextRow(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color,
                 UINT fmt);

// 量一段文本在给定矩形内按 fmt 折行后的实际高度（DT_CALCRECT 的收编版）。
//
// 【为什么要有它】"画一行"已经收进 DrawTextRow，但"先量高度再画"的调用方
// （AI 面板占位页按 \r\n 逐段排版）还得自己写 DrawTextW + DT_CALCRECT，
// 又是一个绕过 ui_draw.cpp 的口子（check_components 的"画一行文本"规则）。
// 这里把"选字体 → DT_CALCRECT → 还原"三步收进来，和 DrawTextRow 成对：
// 量用的是同一个 font，画也是同一个字体，不会出现"量一套画一套"。
// box 给可用的行矩形（宽度决定折行），fmt 由调用方给（DT_WORDBREAK 等）。
int MeasureTextHeight(HDC hdc, HFONT font, const std::wstring& text, const RECT& box, UINT fmt);

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
