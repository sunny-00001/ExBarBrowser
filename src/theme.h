// 主题：颜色令牌 + 字体 + 缩放，唯一视觉来源（对应 ExBar 的 tokens.css）
#pragma once
#include "common.h"
#include "tab_set.h"   // core::kTabColorCount（档数唯一定义）

// 深浅色档位：system = 跟随系统（默认）/ dark = 强制深色 / light = 强制浅色。
// 【为什么在这里而不是 Theme 成员】它是**用户偏好**，住在 Config 里；Theme 只回答
// "当前这一帧是深还是浅"。放到 Theme 上会变成"每个 Theme 实例各存一份偏好"，
// 而 Theme 有 Reload 路径 —— 偏好跟着重建成默认值，用户的选择就丢了。
namespace thememode {
inline constexpr const wchar_t* kSystem = L"system";
inline constexpr const wchar_t* kDark = L"dark";
inline constexpr const wchar_t* kLight = L"light";
}  // namespace thememode

// 深浅色档位 -> "要不要深色"。纯函数，不碰注册表也不读配置，便于单测。
//
// 【为什么要单独抽出来】调用点有三处（Theme::Init、WM_SETTINGCHANGE、
// WM_THEMECHANGED），三处各写一遍 `mode==dark || (mode==system && SystemIsDark())`
// 就会分叉 —— 而分叉的表现是"强制浅色时切一次系统主题又变回深色"，看不出是谁改的。
// 认不出的 mode 一律按 system 处理：配置被手改坏时跟随系统，比固定某一档更不容易出错。
bool ModePrefersDark(const std::wstring& mode);

// 按当前配置（Config::s().themeMode）解析出的深浅色意图。
// 配置项支持热改：改完之后 App::ApplyTheme() 重新 Reload 一次就换配色。
bool ThemePrefersDark();

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
    COLORREF inert{};       // 列表次要字（命令 id / 分组列）
    COLORREF disabled{};
    COLORREF chevron{};
    COLORREF accent{};
    COLORREF editBg{};
    COLORREF editBorder{};
    COLORREF listBg{};
    COLORREF listHot{};
    COLORREF listSel{};
    COLORREF selSolid{};     // 实心强调选中底（弹层高亮行铺满）
    COLORREF selSolidFg{};   // 实心强调选中上的文字色
    COLORREF panelBorder{};  // 浮层外框 1px
    // 设置对话框的四档底（语义见 ui_palette.h 的说明）：
    // 说明面板底 / 左栏当前分类底 / 中栏当前行底 / 行悬停底。
    COLORREF panelBg{};
    COLORREF softSel{};
    COLORREF wash{};
    COLORREF rowHover{};
    // 导航树组间分隔线（explorer 实测值，与 separator 不是同一档，见 ui_palette.h）。
    COLORREF navSep{};
    // 与 Shell 视图边框同色的分界色。用途是"贴着视图边缘画的那条线"——
// AI 面板左边缘用它，就与文件视图自带边框一模一样、看不出额外一条。
// 双栏分割条不用它（那边是实体条 + line 中线，见 splitter96 的说明）。
    COLORREF paneSep{};
    COLORREF danger{};      // 出错（值非法 / 操作被拒）

    // 96 DPI 逻辑圆角（用 S() 换算）。以前只有导航栏按钮有圆角（写死在
    // nav_bar_draw.cpp 的私有 FillRound 里），其余栏是直角 —— 统一到这里。
    int radiusBtn96 = 4;
    int radiusPopup96 = 6;

    // ---- 双栏分割 ----
    // 【实体分隔条，遮住两侧视图边框】
    //
    // 这一处被返工三次，前两次都错在同一件事：想让"缝里只露底色"或"填同色"，
    // 结果两栏 Shell 视图自带的那条 1px 边框始终露在外面 —— 它是 Shell 画的，
    // 我们关不掉。只要它可见，无论我们再画什么都不可能只有一条。
    //
    // 业界做法（VS Code 的 sash / DOpus / JetBrains 的 divider）都是**实体条**：
    // 分隔条是一块有宽度的区域，把两侧内容各压一点，视觉上只有一个元素。
    // 这里照做：
    //   · 两栏之间留 splitter96（6px）的缝；
    //   · 分隔条窗口比缝再各宽 1px（hit = gap + 2），**把两侧视图边框压在自己底下**；
    //   · 窗口内填 bg（与两栏内容同色，看不出边界），中线画 1px 分界线。
    // 于是：视图内容 | bg | 1px 线 | bg | 视图内容 —— 只有一条线。
    //
    // splitter96 是缝宽（布局用），splitterHit96 是**实际窗口宽**（= 缝 + 两侧各 1px）。
    static constexpr int splitter96 = 6;
    int splitter() const { return S(splitter96); }
    // 窗口宽：缝 + 左右各压住 1px 的视图边框。
    int splitterHit() const { return splitter() + 2; }

    // ---- 侧栏边界（导航 | 文件、文件 | AI 面板）----
    //
    // 【为什么不复用 splitter96】双栏之间**留缝**是必要的：两栏是两个独立视图，
    // 中间那块不属于任何一栏的像素得有明确归属。侧栏不一样 —— 导航树与文件区
    // 是两种内容，交界处本来就该由一条线分开，没有"让谁占 6px"的问题。
    //
    // 【为什么是 6px 可抓、只画 1px 线】拖动需要手感，但常驻一条 6px 的厚带
    // 会把 explorer 式的"一条细线"变成"两块之间夹一条灰条"。所以抓取窗口
    // 6px 压在这条线上，**平时只画 1px、其余用两侧各自的底色**（导航栏与文件
    // 列表的底本来就同色，见 ui_palette.h 的 viewBg 说明）。鼠标移上来只把
    // 那 1px 中线换成 accent 并加粗（不整条变色）—— 悬停反馈不改变任何宽度，
    // 离开时下一个 WM_PAINT 全量重画，不会残留（见 split_bar.cpp 的 Paint）。
    static constexpr int edgeHit96 = 6;
    int edgeHit() const { return S(edgeHit96); }
    // 悬停/拖动时只把中线加粗并换 accent：这是"能拖"的唯一提示（与双栏分割条同手法）。
    // 这里不新增颜色令牌 —— accent 已经是"可交互/强调"的唯一取值。

    // ---- 栏高令牌（96 DPI 逻辑值）----
    // 【为什么必须在这里】这四条高度以前各写一份：标签栏用 tab_strip.h 的
    // kTabBarHeightDp(26)，导航栏 / 书签栏 / 状态栏各自在 Create() 里写
    // theme.S(30/26/24)，而 app_layout.cpp 的 Layout() 与 app.cpp 的建树高预算
    // 又把这四个数**各自重算一遍**（后者注释里甚至写着"公式必须与 Layout() 保持一致"）。
    // 同一个数五处，调整栏高要改五遍；漏一处的表现是"栏边露出一条视图"或
    // "标签被裁掉一半"，且只在特定 DPI 下才看得出来。
    // 四条栏高是**纯逻辑值**（不随实例变），所以是 static constexpr：
    // tab_strip.h 的 kTabBarHeightDp 要能与之做 static_assert 比对。
    static constexpr int barTab96 = 26;
    static constexpr int barNav96 = 30;
    static constexpr int barBookmark96 = 26;
    static constexpr int barStatus96 = 24;
    // 运行值（96 DPI 逻辑像素）。默认等于上面四个编译期基准；Config 载入后
    // 由 App::ApplyBarHeights 把设置项推进来。保留实例值而不是让 Theme 直接
    // 读全局 Config，是为了让 Theme 仍是纯渲染对象（单测可以独立构造）。
    int barTab96_ = barTab96;
    int barNav96_ = barNav96;
    int barBookmark96_ = barBookmark96;
    int barStatus96_ = barStatus96;
    // 参数均为 96 DPI 逻辑像素；范围由 settings_bounds/config_io 在读写两端夹过，
    // 这里只挡住 0/负数（手改配置且不重启时也不会把整条栏抹掉）。
    void SetBarHeights(int tab, int nav, int bookmark, int status) {
        if (tab > 0) barTab96_ = tab;
        if (nav > 0) barNav96_ = nav;
        if (bookmark > 0) barBookmark96_ = bookmark;
        if (status > 0) barStatus96_ = status;
    }
    int barTab() const { return S(barTab96_); }
    int barNav() const { return S(barNav96_); }
    int barBookmark() const { return S(barBookmark96_); }
    int barStatus() const { return S(barStatus96_); }

    // 四条栏都可见时占掉的垂直空间（= 内容区顶部的总偏移）。
    // 布局与启动期的"树建多高"必须用**同一个**公式：两处各算一遍，差 1~2px 就会
    // 让导航树在启动时先按错高度建一次、再被 Resize（用户看到的那一帧是空白树）。
    int ChromeHeight(bool showTab, bool showNav, bool showBookmark, bool showStatus) const {
        int h = 0;
        if (showTab) h += barTab();
        if (showNav) h += barNav();
        if (showBookmark) h += barBookmark();
        if (showStatus) h += barStatus();
        return h;
    }

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
    HBRUSH brSelSolid{};
    HBRUSH brAccent{};
    HBRUSH brViewBg{};   // 视图区容器窗口类的背景刷（比窗口活得久）
    // 最近一次 ApplyToViewArea 的窗口。Shutdown 删 brViewBg 之前要把类背景刷
    // 复位，否则类里留着一个已删除的句柄（Reload 期间是危险窗口）。
    // 存 HWND 而不是每次 FindWindow：本函数在 UI 线程，窗口句柄稳定。
    HWND viewArea_{};

    HFONT font{};
    HFONT fontBold{};
    // 等宽字体：序列键 / 命令 id / 序号列要等宽对齐（正文用 UI 字体）。
    // 列表用 fontMono（与正文同号），输入区缓冲用 fontMonoBig（大一号）。
    HFONT fontMono{};
    HFONT fontMonoBig{};
    // 字体代次：每次真正重建字体 +1。原生控件（地址栏 EDIT）用它判断要不要
    // 重新 WM_SETFONT —— 不能比较 HFONT 句柄值：GDI 删除对象后会把同一句柄值
    // 分配给新字体，"句柄相同"时旧句柄其实已失效，EDIT 会继续拿失效字体绘制。
    unsigned fontGeneration = 0;

    // 把一个"纯背景窗口"（视图区容器）的类背景刷换成本主题的 bg。
    // 以前 app.cpp 的 OnCreate 与 app_layout.cpp 的 ApplyTheme 各写一遍
    // CreateSolidBrush + SetClassLongPtrW，且都要自己管 viewBg_ 的释放。
    // 窗口类持有的画刷必须比窗口活得久 → 由省 Theme 唯一持有，随 Reload 一起换。
    void ApplyToViewArea(HWND area);

    // dpi 变化时整体重建（先 Shutdown 再 Init）
    // dpiOverride > 0 时用它（WM_DPICHANGED 的 wParam 才是权威值）；0 则自动探测。
    void Init(HWND hwnd, int dpiOverride = 0, bool skipFonts = false);
    void Shutdown(bool keepFonts = false);
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
