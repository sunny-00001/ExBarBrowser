// 色值令牌表（唯一来源）。
//
// 【来历】色值最初从 mod 的 `src/parts/001-eb.cpp` 的 ThemeColors 原样搬来，
// 目的是"换栈前后像素级一致"。现在这些值已成为本工程自己的基线 —— 改它就是在
// 改本工程的观感，不再需要与 mod 对账（工程已独立，见 docs/NAMING.md）。
//
// 【纪律】不要在栏/弹层里自己调色。所有颜色经 Theme（theme.h）流转 ——
// Theme::Init 从这里取令牌，再把它们变成 COLORREF 与 HBRUSH 供组件使用。
#pragma once
#include "common.h"

struct UiPalette {
    // ---- 已被 Theme 消费（改动生效）----
    // viewBg 是唯一"底"色：既是 Shell 文件列表的底，也是所有自绘 chrome 的底。
    // 曾经这里有单独的 bar 令牌（浅 #f3f3f3 / 深 #1e1e1e），与 Shell 列表的
    // viewBg（浅 #ffffff / 深 #141414）不同 —— 表现就是"自绘外壳与系统列表
    // 有色差"。工具栏/标签栏/书签栏/状态栏没有独立的视觉理由比列表深一档，
    // 它们统一取 viewBg；要调底色调这里一处即可，深浅自动一致。
    COLORREF viewBg;                                         // 视图容器底 / Shell 列表底 / 所有自绘栏底
    COLORREF text, hover, pressed, disabled, separator;      // 栏基础色
    COLORREF editBg, editBorder;                             // 编辑框底/边框
    COLORREF listBg, listSelBg, listHotBg;                   // 下拉列表底/选中/悬停
    COLORREF chevron;                                        // 面包屑展开箭头
    COLORREF accent, dimText;                                // 强调色 / 次要文字
    COLORREF inert;                                          // 列表里"次要一行"的字（命令 id / 分组）
    // 实心强调选中（弹层高亮行：整行铺强调色 + 反色文字）。
    // 与 listSelBg 是两件事：listSelBg 是列表控件的淡蓝选中（不铺满行），
    // 弹层这一行要"整行实心"才看得出选中的是哪一行 —— 参考图里就是铺满的蓝。
    COLORREF selSolidBg, selSolidFg;
    COLORREF panelBorder;                                    // 浮层外框 1px
    // 设置对话框用的四档"底"。它们与列表色是**不同的东西**：
    //   panelBg  右栏（说明面板）的底 —— 与中栏内容区有可见分界，否则三栏看着是两块
    //   softSel  强调色的淡化底 —— 左栏当前分类 / 下拉当前项
    //   wash     强调色更淡的底 —— 中栏当前行（比 softSel 更弱：当前行不该抢过当前分类）
    //   rowHover 行悬停底 —— 必须比 wash 更淡，否则"悬停"看着像"选中"
    // 强弱顺序是 wash < softSel，两者不可互换：左栏是"页签"，中栏是"行"，
    // 页签的选中态要更明确，行只是"我现在看的是这一条"。
    COLORREF panelBg, softSel, wash, rowHover;
    // 导航树**组间分隔线**专用色。
    //   为什么不是 separator：深色下分隔线是 0x383838（explorer 逐像素实测），
    //   而 separator 是 0x3F3F3F —— 两者差一档，用错一档线就比 explorer 浅。
    //   浅色下 explorer 的分组线与栏分隔线同值（0xD0D0D0），所以浅色这一档
    //   就取 separator 的值，不另造。
    COLORREF navSep;
    // 双栏分割缝的颜色。**刻意取 Shell 视图边框的实测值，而不是 separator**：
    // 左栏文件视图自带一条右缘边框（浅色 #646464 / 深色 #4A4A4A 一档），缝若用
    // separator（#D0D0D0 / #3F3F3F）就会与它明暗不同 —— 并排两像素看着是两条线。
    // 取同一个色，"视图边框 + 缝"合成一条，这就是"双栏之间只有一条线"的落地方式。
    COLORREF paneSep;
    // 出错色（值非法 / 操作被拒）。之前没有这个令牌，于是"输入不合法"
    // 只能靠一句状态栏文字表达 —— 而框还是那个框，看不出哪里出了问题。
    COLORREF danger;
};

inline const UiPalette& PaletteFor(bool dark) {
    // 指定初始化器（C++20）：**字段顺序无关**。
    // 这里原来是位置式初始化 —— 20 个连续 COLORREF 靠逗号对齐，
    // 插一个字段、删一个字段都会让后面全部错位，而错位的表现是"某个颜色莫名其妙变了"，
    // 比编译错误难查得多。
    static const UiPalette light = {
        // 实测：浅色文件区 #ffffff（系统 COLOR_WINDOW）。底只有这一个，工具栏同色。
        .viewBg = 0xFFFFFF,
        .text = 0x1A1A1A,       .hover = 0xE5E5E5,     .pressed = 0xD0D0D0,
        .disabled = 0xB0B0B0,   .separator = 0xD0D0D0,
        .editBg = 0xFFFFFF,     .editBorder = 0xC8C8C8,
        .listBg = 0xFFFFFF,     .listSelBg = RGB(204, 232, 255), .listHotBg = 0xE8E8E8,
        .chevron = 0x666666,
        .accent = RGB(0, 120, 215), .dimText = 0x606060,
        // 列表次要字比 dimText 更淡 —— 它与主信息（描述）同屏出现，
        // 太深会抢主信息的视线（参考 mod 的 listInertText 口径）。
        .inert = 0x9A9A9A,
        // 实心强调选中：与系统强调色同源（Win11 浅色下的选中行就是这抹蓝）。
        .selSolidBg = RGB(0, 120, 215), .selSolidFg = 0xFFFFFF,
        .panelBorder = 0xA0A0A0,
        // 设置对话框四档底（浅色）：右栏比内容区略亮一点以示"这是说明区"；
        // softSel 比 wash 浓一档（左栏是页签，页签的选中要更硬）。
        .panelBg = 0xFBFBFC, .softSel = RGB(204, 232, 255),
        .wash = RGB(234, 244, 253), .rowHover = 0xFAFAFA,
        // 浅色下导航树分组线与栏分隔线同值（explorer 实测口径一致）。
        .navSep = 0xD0D0D0,
        // 浅色下 Shell 视图右缘边框实测 #646464（截图 x=547 处）。
        .paneSep = 0x646464,
        .danger = RGB(0xC4, 0x2B, 0x1C),
    };
    static const UiPalette darkTheme = {
        // 实测（2026-11-22 深色截图）：深色文件区/列表底 #191919。
        // 令牌此前是 #141414，与当前 Shell 实际列表底差一档 —— 深色下工具栏
        // 比列表偏深，正是"深浅还是有色差"的根因。底只有这一个令牌，改这里
        // 工具栏/视图容器同步，与列表齐平；浅色 #ffffff 两侧本就同值，无需动。
        .viewBg = 0x191919,
        .text = 0xEEEEEE,       .hover = 0x3A3A3A,     .pressed = 0x2E2E2E,
        .disabled = 0x606060,   .separator = 0x3F3F3F,
        .editBg = 0x2A2A2A,     .editBorder = 0x555555,
        .listBg = 0x202020,     .listSelBg = RGB(76, 159, 255),  .listHotBg = 0x353535,
        .chevron = 0xBBBBBB,
        .accent = RGB(76, 160, 255), .dimText = 0x9A9A9A,
        .inert = 0x8A8A8A,
        // 深色下的强调色偏亮 —— 实心选中行用**黑字**，否则白字压在 #4CA0FF 上
        // 只有 2.9:1 的对比度，读不清（Windows 11 深色同样是黑字）。
        .selSolidBg = RGB(76, 160, 255), .selSolidFg = 0x000000,
        .panelBorder = 0x3F3F3F,
        // 深色下的四档底：说明区比内容区略深（浅色下是略亮 —— 深浅两套的"分界"
        // 方向相反，但目的一样：让右栏看得出是另一块区域）。
        .panelBg = 0x1E1E1E, .softSel = RGB(31, 62, 92),
        .wash = RGB(26, 44, 62), .rowHover = 0x1F1F1F,
        // 深色下的导航树分组线：**explorer 逐像素实测 = 0x383838**，
        // 与 separator（0x3F3F3F）不是同一个值，别混用。
        .navSep = 0x383838,
        // 深色下同一条边框比浅色暗一档。
        .paneSep = 0x4A4A4A,
        // 深色下的出错色比浅色版亮一档：#C42B1C 压在深底上几乎看不见。
        .danger = RGB(0xE0, 0x6C, 0x75),
    };
    return dark ? darkTheme : light;
}

// 标签分类色板（8 档：0=无颜色，1..7=红/橙/黄/绿/蓝/紫/灰）。
//
// 与 UiPalette 分开：这是"分类标记"，不是 UI 基础令牌 —— 深浅各一套是因为
// 同一色值在浅色标签上过重、在深色标签上过暗（详见 theme.cpp 的 TabColorFill）。
// 放在这里是为了让"色值不与实现混写"：theme.cpp 不再有硬编码 RGB。
struct TabPalette {
    COLORREF colors[8];
};

inline const TabPalette& TabColorsFor(bool dark) {
    static const TabPalette light = {
        {0,
         RGB(0xC0, 0x39, 0x2B), RGB(0xC4, 0x70, 0x0C), RGB(0xA8, 0x8A, 0x00),
         RGB(0x2E, 0x7D, 0x32), RGB(0x00, 0x78, 0xD7), RGB(0x6A, 0x3D, 0x9A),
         RGB(0x60, 0x60, 0x60)}};  // 黄在浅底上压暗，否则看不见
    static const TabPalette darkTheme = {
        {0,
         RGB(0xE0, 0x5A, 0x5A), RGB(0xE0, 0x93, 0x3F), RGB(0xD6, 0xC0, 0x4A),
         RGB(0x5F, 0xB8, 0x6A), RGB(0x4C, 0xA0, 0xFF), RGB(0xA0, 0x7C, 0xE0),
         RGB(0x8A, 0x8A, 0x8A)}};
    return dark ? darkTheme : light;
}
