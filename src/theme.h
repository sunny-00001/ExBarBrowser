// 主题：颜色令牌 + 字体 + 缩放，唯一视觉来源（对应 ExBar 的 tokens.css）
#pragma once
#include "common.h"
#include "tab_set.h"   // core::kTabColorCount（档数唯一定义）

struct Theme {
    bool dark = true;
    int dpi = 96;

    COLORREF bg{};      // 内容区/视图底
    COLORREF bar{};     // 工具栏底
    COLORREF barAlt{};  // 输入框/次级底
    COLORREF hover{};
    COLORREF pressed{};
    COLORREF line{};
    COLORREF text{};
    COLORREF sub{};
    COLORREF disabled{};
    COLORREF chevron{};
    COLORREF accent{};
    COLORREF editBg{};
    COLORREF editBorder{};
    COLORREF listBg{};
    COLORREF listHot{};
    COLORREF listSel{};

    // 96 DPI 逻辑圆角（用 S() 换算）。以前只有导航栏按钮有圆角（写死在
    // nav_bar_draw.cpp 的私有 FillRound 里），其余栏是直角 —— 统一到这里。
    int radiusBtn96 = 4;
    int radiusPopup96 = 6;

    // ---- 图标尺寸令牌（96 DPI 逻辑值）----
    // 为什么必须在这里：字形表（icon_glyph）管"哪个码位"、DrawGlyph 管"怎么画"，
    // 唯独"画多大"此前没有归属 —— 7 个裸数字散在 6 个文件里（S(9)/S(11)/S(12)/S(14)/S(16)），
    // 同一行里的三个图标可能来自三个不同数字。
    // 这里只保留 4 档，按**角色**分，不按"哪个栏想画多大"：
    //   iconChevron96  下拉/展开箭头（面包屑每级、书签栏分类、弹层展开器、分裂箭头）
    //   iconGlyph96    栏内小字形（关闭 ✕ / 新建 ＋ / 溢出箭头 / 锁定 / 状态栏段图标）
    //   iconCompact96  紧凑位置的条目图标（导航按钮、面包屑前导文件夹）
    //   iconSmall96    列表/标签条目图标（标签栏 / 书签栏 / 弹层行）
    // 新增栏位时选一档，不要新造数字；确实要新的档位，先在这里加并写清差异理由。
    //
    // 最小值由 ui::DrawGlyph 内部统一夹（8 物理像素），这里不重复夹一次 ——
    // 两个下限就是两套口径，正是本工程删掉 FontPx/Scale/IconSize 的理由。
    // 【曾经的坑】导航栏分裂箭头传的是 S(4)（96dpi = 4px），被那个下限静默抬成 8px，
    // 于是 96%~168% DPI 区间里按钮从 24px 长到 42px、箭头一动不动。它现在走
    // iconChevron 档（10px > 下限），"传进去的值"与"画出来的值"不再有落差。
    int iconChevron96 = 10;
    int iconGlyph96 = 12;
    int iconCompact96 = 14;
    int iconSmall96 = 16;

    // ---- 标签颜色（下标语义见下）----
    // core::TabSet 是零 Win32 的，只存**下标**（TabRef::color）；下标 -> COLORREF
    // 的换算只在这里做一次，否则"标签配色"会变成第二套色表（R11）。
    // 0 = 不设颜色（不是"黑色"）—— 菜单第一行"无颜色"就落在这里。
    // 声明在 Theme 里、实现在 theme.cpp：颜色必须与深浅色主题配套，
    // 深色底下压暗一档的蓝在深色标签上根本看不见。

    HBRUSH brBg{};
    HBRUSH brBar{};
    HBRUSH brBarAlt{};
    HBRUSH brHover{};
    HBRUSH brPressed{};
    HBRUSH brEdit{};
    HBRUSH brList{};
    HBRUSH brListHot{};
    HBRUSH brListSel{};
    HBRUSH brAccent{};
    HBRUSH brViewBg{};   // 视图区容器窗口类的背景刷（比窗口活得久）

    HFONT font{};
    HFONT fontBold{};

    // 把一个"纯背景窗口"（视图区容器）的类背景刷换成本主题的 bg。
    // 以前 app.cpp 的 OnCreate 与 app_layout.cpp 的 ApplyTheme 各写一遍
    // CreateSolidBrush + SetClassLongPtrW，且都要自己管 viewBg_ 的释放。
    // 窗口类持有的画刷必须比窗口活得久 → 由省 Theme 唯一持有，随 Reload 一起换。
    void ApplyToViewArea(HWND area);

    // dpi 变化时整体重建（先 Shutdown 再 Init）
    // dpiOverride > 0 时用它（WM_DPICHANGED 的 wParam 才是权威值）；0 则自动探测。
    void Init(HWND hwnd, int dpiOverride = 0);
    void Shutdown();
    void Reload(HWND hwnd, int dpiOverride = 0);

    int S(int px) const { return MulDiv(px, dpi, 96); }
    // 令牌版：圆角由 Theme 统一给出，绘制处不再写裸像素。
    int radiusBtn() const { return S(radiusBtn96); }
    int radiusPopup() const { return S(radiusPopup96); }
    // 图标尺寸令牌版：调用方一律用这几个，不再各自 S(裸数字)。
    int iconChevron() const { return S(iconChevron96); }
    int iconGlyph() const { return S(iconGlyph96); }
    int iconCompact() const { return S(iconCompact96); }
    int iconSmall() const { return S(iconSmall96); }

    // 标签颜色：色板是固定 8 档（0 = 无颜色），标签只存下标。
    // 为什么是"预定义色板"而不是取色器：标签颜色是"分类标记"，不是设计工具 ——
    // 自由取色会让人给每个标签调一个近似色，最后既认不出分组、深色下还看不清。
    // 8 档足够区分（VS Code / Chrome 的标签组也都在这个量级）。
    // 档数的唯一定义在 core::kTabColorCount：校验层（TabSet::SetColor、读盘夹取）
    // 也要用它，而校验层拿不到 Theme —— 定义在这里会逼出三处硬编码 `> 7`。
    static constexpr int kTabColorCount = core::kTabColorCount;
    // 色板第 i 档在**标签底色**上的填充色。i=0 或越界返回 CLR_INVALID（不填）。
    COLORREF TabColorFill(int idx) const;
    // 同一档的前景条色（激活标签底部那条 accent 的替代色，用于一眼认出颜色）。
    COLORREF TabColorBar(int idx) const;
    // 色板名字（右键菜单的二级列表用）。
    static const wchar_t* TabColorName(int idx);
};

// 进程级深色模式（uxtheme 未文档化导出，Win10 1903+）。
// 不调用它时，IExplorerBrowser 托管的 Shell 视图会固定浅色（实测白底）。
// 取不到导出就静默降级为"仅工具栏深色"，不影响功能。
void ApplyProcessDarkMode(bool dark);
void AllowDarkForWindow(HWND hwnd, bool dark);
