// 自绘下拉弹层（替代系统 TrackPopupMenu）
//
// 对应 ExBar mod 的 ebpopup（Exbar.cpp 18356-19314）：图标 + 24px 行高 +
// 悬停/选中底色 + 深色边框 + 文件夹行右侧统一 chevron。系统菜单无法带图标、
// 配色也跟不了主题，所以这里用「弹出窗口 + owner-draw ListBox」自己画。
//
// 两种形态：
//   ShowPopupList  —— 平铺列表（历史/视图/下拉等），点击即选中。
//   ShowFolderTree —— 文件夹树（面包屑分隔符下拉），点击行首 chevron 就地
//                     展开/收回子节点（与 Exbar.cpp TreeNode 的 treeMode 一致），
//                     而不是向右弹出新菜单。
//
// 【再点即收（toggleKey）】所有弹层同一契约：**弹层开着时点它的锚点，收起；不重开**。
//
// 实现靠**鼠标捕获**，不是时间窗（见 popup_toggle.h 解释为什么时间窗必错）：
// 弹层活着期间列表持有捕获，于是"关掉它的那一下点击"被列表自己吃掉，
// 锚点根本收不到 —— 不会调第二次 Show*，菜单不会重弹。这是 TrackPopupMenu /
// 组合框下拉 / 资源管理器的同一套做法。
//
// 调用方只需给每个锚点传一个稳定且互不相同的 key（`nav.crumb.3`、`nav.drop`、
// `tab.menu.1`…）。key 用于两件事：
//   1. Show* 内部兜底 —— 同键弹层仍开着时，非鼠标路径（键盘/命令）的重入直接返回；
//   2. 有"可见状态"的调用方（面包屑箭头 › / ⌄）用 popup::IsOpen 决定形态。
// 不传（nullptr）= 不存在"同一个锚点"，不参与判定（一次性命令面板）。
//
// 带索引的位置必须把索引编进去：`nav.crumb.3` 与 `nav.crumb.5` 是两个不同锚点，
// 否则点第 3 段收起后再点第 5 段会被误判。
#pragma once
#include "common.h"
#include "icon_glyph.h"
#include "theme.h"

struct PopupRow {
    std::wstring label;
    std::wstring path;
    HICON icon = nullptr;   // 调用方拥有；弹层不销毁
    bool folder = false;    // 行右端画 chevron（表示还有下一级）
    bool separator = false;
    bool disabled = false;
    int data = -1;
    // 字形图标：与 icon 二选一。菜单动作不需要真的 CreateIcon —— 直接复用
    // ico::IconId 登记表 + ui::DrawGlyph，比"把字形烘成 HICON"少一套生命周期。
    // 放在最后：老调用方的 7 字段聚合初始化不必改。
    ico::IconId glyph = ico::IconId::None;
    // 勾选态：**独立字段**，不再拼进 label。
    //
    // 【为什么不能拼进文字】旧口径是 label = "✓ " + 文案 / "   " + 文案。
    // 三个问题：
    //   1. "✓" 走的是**正文字体**（Segoe UI）里的 U+2713，不是系统的勾形字形 ——
    //      粗细、弧度跟旁边 Segoe Fluent Icons 的图标不是一个体系，看着就是"外挂"的。
    //   2. 未勾选行用**三个 ASCII 空格**占位。Segoe UI 的空格比 "✓" 窄，
    //      于是勾选行与未勾选行的文字**横向对不齐**（差一个勾号宽）。
    //   3. 勾号混在文字里，弹层量宽时把它算进文案，同一列的文字宽度忽宽忽窄。
    // 现在勾号走**独立的一列**（见 HasCheckedRow / PopupCheckColWidth），
    // 与 Win32 / Windows 11 Fluent / ARIA menuitemcheckbox 同一口径。
    bool checked = false;
    // 行右端的快捷键文案（"Ctrl+Shift+T" / "Shift+Left" / 序列 "t b"）。空 = 不画。
    //
    // 【为什么不拼进 label】与 checked 是同一类问题的三个变体：
    //   1. label 是**单行 + DT_END_ELLIPSIS**，键位拼进去会跟文案一起被截断 ——
    //      而菜单最长的恰恰是"复制路径（含分类内）  Ctrl+Shift+C"这种行；
    //   2. 本弹层的 type-ahead 按 label 前缀定位，键位混入后敲 `c` 会命中
    //      "复制  Ctrl+C" 这种"文案并不以 c 开头"的行；
    //   3. 键位必须**右对齐成一列**（Win32 菜单 / Explorer / Win11 Fluent /
    //      VS Code 都是右侧独立列），拼在 label 里只能靠空格凑，字体一变就歪。
    // 值由 ui::Menu 在 Show 前按"命令规格"现取（见 menu.h 的 SetShortcutProvider），
    // 弹层只负责画，不认识键位表。
    // 放在最后：老调用方的 7/8/9 字段聚合初始化不必改。
    std::wstring shortcut;
};

// 弹层要不要留"勾选列"：只要有一行可勾选就留**整列**（Win32 菜单的规矩）。
//
// 未勾选的行在同一列里留空 —— 不用空格占位（见 PopupRow::checked 的说明）。
// 整列都没有勾选时，这一列不出现，菜单左边距跟从前一样（不留空槽）。
inline bool HasCheckedRow(const std::vector<PopupRow>& rows) {
    for (const auto& r : rows)
        if (!r.separator && r.checked) return true;
    return false;
}

// 勾选列宽 = 图标槽宽。勾号按图标的同一尺寸画，两列等宽才不别扭。
inline int PopupCheckColWidth(int iconS) { return iconS; }

// 弹层的初态：打开时预选哪一行、要不要滚到它。
//
// 【为什么要这个】菜单/下拉在三种场景里"打开就是空的"是明显的坏味道：
//   · 地址栏历史下拉 24 条，当前目录可能在第 18 条 —— 用户得自己找；
//   · 标签溢出菜单里当前标签可能滚出视野；
//   · 状态栏段的勾选列表里看不出哪几项是开的。
// 资源管理器、Opus、TC、XYplorer 的下拉都会把"当前项"预选并滚进视野。
//
// 用**行索引**而不是 PopupRow::data：data 是调用方的动作编号（有 100/2000 这类
// 分段偏移），拿它当行号会错位；分隔行也不占 data。
struct PopupInitial {
    int selectRow = -1;   // 预选行号（-1 = 不预选）。落在分隔/禁用行时自动跳过
    bool scrollTo = true; // 是否把它滚进视野
};

// 行内拖拽重排（弹层的**可选能力**）。
//
// 【为什么要有】书签栏顶层那条自绘横条早就支持拖动重排，而**分类里的条目**
// 只能点、不能组织 —— 同一个界面里两套规则，用户第一次在分类里拖不动时
// 会以为功能坏了。分类内容是用弹层呈现的，所以这条能力必须落在弹层上。
//
// 【为什么不另写一份弹层】弹层的模态窗口、鼠标捕获语义、type-ahead、懒加载
// 都只有一份（popup_list.cpp）。给分类另起一个弹层 = 把整套机制复制一遍。
// 所以这里把"把一行拖到另一行之前"做成弹层的**可选通道**：弹层只管
// "拖的是第几行、落在第几行之前"，**不认业务**（它不知道什么是书签）。
//
// 【坐标约定】from / to 都是**弹层自己的行号**（分隔行也占号），宿主自己
// 映射到 id。to 的含义是"插到第 to 行之前"，按**被拖行仍在列表里**的坐标
// 给出（与 Windows 列表/资源管理器一致）：所以 to 可能等于行数（末尾），
// 也可能正好等于 from（原地不动）。
struct PopupReorder {
    // 这一行能不能被拖（不给 = 全都能拖）。
    std::function<bool(int row)> canDrag;
    // 拖放提交：把第 from 行移到第 to 行之前。
    std::function<void(int from, int to)> onReorder;
    // 落点之后由宿主重建行内容，弹层**就地刷新**（不关窗口）。
    // 宿主把新行填进 out；返回 false = 不刷新（弹层保持旧内容）。
    //
    // 为什么需要它：重排后行序变了，弹层若还拿着旧行，用户看到的是"没动"。
    // 宿主重建时**必须**用新的顺序填 out，否则刷新等于没刷新。
    std::function<bool(std::vector<PopupRow>* out)> rebuildRows;
    // 行内右键：弹层把命中的行号交回宿主，然后**自己先关**。
    //
    // 为什么不让弹层直接弹菜单：右键菜单本身是模态的，在弹层的模态循环里
    // 再弹一层 = 两个模态循环嵌套（捕获、焦点、Esc 全乱）。所以约定是
    // "弹层交回行号并关闭，宿主在自己的循环里处理"。
    std::function<void(int row)> onRowRightClick;
    // 拖出弹层（可选）：宿主自行把屏幕点映射到自己的落点并给反馈。
    // 弹层只负责在"拖拽中且光标离开列表"时把 row/屏幕点交回来：
    //   · onDragOutPreview：每次移动到弹层外调用；收到 {-1,-1} 表示回到列表内/拖拽取消，
    //     宿主应清掉自己的预览。
    //   · onDragOut：在弹层外松手时调用一次，宿主提交动作；弹层随后关闭。
    // 为什么不给弹层加"书签栏"概念：它是通用组件，只认行号与屏幕坐标。
    std::function<void(int row, POINT screen)> onDragOutPreview;
    std::function<void(int row, POINT screen)> onDragOut;
    // 外部拖拽移交（可选）：弹层打开的瞬间就有一场"从外面拖进来的"拖拽在进行
    // （书签栏拖动悬停分类 → 自动弹出分类内容 → 拖拽继续），不是用户在弹层里
    // 重新按下的。置 true 时：进场即在拖拽态（没有"按下"这一步），起始行号
    // **-1 = 外部项**，onReorder / onDragOut* 回调里的 from 都会收到 -1，
    // 由宿主映射到外部条目；松手没压在有效落点上 = 加入末尾（宿主语义）。
    bool allowExternalFrom = false;
};

// 拖拽落点：光标压在第 row 行上时，插入线落在哪一行之前。
//
// 【上半 / 下半】同一行里，光标压上半 → 插到这行**之前**；压下半 → 插到这行**之后**。
// 与 Windows 列表 / 资源管理器的拖放一致：拖过一行中线才换边，不换边就看不出
// "到底插前面还是插后面"。
//
// 【坐标口径】返回的 to 按"**被拖行仍在列表里**"给出（所以 to 可能 > 实际位置）。
// 宿主把它交给 bmk.move 的 toIndex —— 那边自己按"移除被拖行"折算（`to > from` 时
// 减一）。两处各折算一次就会差一位，所以这里**不**折算。
//
// 【边界】row 越界（光标不在任何行上）→ -1 = 没有落点。
inline int PopupDropIndex(int row, bool lowerHalf, int rowCount) {
    if (row < 0 || row >= rowCount) return -1;
    return lowerHalf ? row + 1 : row;
}

// 选中行 → 动作号。
//
// 【必须用"当前显示的那份"行】弹层支持就地重建行（分类内容弹层拖完一行后，
// 宿主重读数据再重建）。重建之后行序变了，而动作号是**构建时**定的 ——
// 重建后的新行带着新动作号。若还用重排之前那份行去查，用户拖完之后点的
// 那一行会被映射成**旧顺序**的动作号：点 A 打开 B、点 B 打开 C。
//
// 纯函数是为了能单测：这条映射错了不报错，只是"打开了隔壁那一项"。
inline int PopupActionFor(const std::vector<PopupRow>& rows, int pick) {
    if (pick < 0 || pick >= static_cast<int>(rows.size())) return -1;
    return rows[static_cast<size_t>(pick)].data;
}

// 文件夹树节点（对应 Exbar.cpp 的 TreeNode）。
// 顶层节点由调用方构造；子节点由 below 的 children 回调懒加载创建。
// icon 走 ico::Cached，进程内共享、弹层只借不销毁；icon 为空时用 glyph 兜底。
struct TreeNode {
    std::wstring label;
    std::wstring path;
    HICON icon = nullptr;
    int depth = 0;             // 缩进层级（弹层设置）
    int parent = -1;           // 父节点在 tree 中的索引（弹层设置）
    std::vector<int> children; // 子节点索引（弹层设置）
    bool hasChildren = true;   // 是否画 chevron（首次展开后发现是叶子会置 false）
    bool expanded = false;     // 展开态：chevron 画 down(dir=1)，收起画 left(dir=0)
    bool loaded = false;       // 子项是否已加载
    // 字形兜底：与 PopupRow::glyph 同一套 ico::IconId 登记表，icon 为空时画它。
    ico::IconId glyph = ico::IconId::None;
};

// 懒加载回调：给定文件夹 path，返回其直接子文件夹节点。
// 只需填 label/path/icon/glyph/hasChildren；depth/parent/children 由弹层接管。
// icon 来自 ico::Cached（借用，不移交所有权）。
using TreeChildrenFn = std::function<std::vector<TreeNode>(const std::wstring& folder)>;

// 行高规则（纯函数，便于单测 —— 不依赖窗口/DPI/ListBox）：
// 分隔线只占 sepPx，普通条目占 itemH。
inline int PopupRowHeight(const PopupRow& row, int itemH, int sepPx) {
    return row.separator ? sepPx : itemH;
}

// 前 n 行的累计高度（窗口尺寸计算用）。n 超过行数时按行数算。
inline int PopupTotalHeight(const std::vector<PopupRow>& rows, int n, int itemH, int sepPx) {
    if (n > static_cast<int>(rows.size())) n = static_cast<int>(rows.size());
    int h = 0;
    for (int i = 0; i < n; ++i)
        h += PopupRowHeight(rows[static_cast<size_t>(i)], itemH, sepPx);
    return h;
}

// 「这一下松开是不是打开菜单那一次点击的收尾」——纯函数，可单测。
//
// ## 为什么需要它（这是"菜单一闪而过"的真因）
//
// 导航栏各下拉是在**按下**时弹出的（与资源管理器一致：按下即出，用户可以一路
// 按住拖到想要的项上再松手）。弹层一创建就 `SetCapture(list)`，于是**同一次点击
// 的松开**（WM_LBUTTONUP）会送到列表，而光标还停在锚点上 —— 锚点在列表之外。
//
// 旧逻辑把"UP 落在客户区外"一律当成"点了外面，该收起"，于是菜单在松手瞬间
// 自己关掉，用户看到的就是"一闪而过"。可这一次 UP 根本不是新点击，它是开菜单
// 那次点击的**后半截**。
//
// TrackPopupMenu 的处理方式正是：先等这一下松开，之后才把点击当作"关闭"。
//
// 判据：只有在**开菜单之后再没有按下过**（`downInList`/`downOutside` 都是 false）、
// 且这笔 UP 落在客户区外时，才认定它是"开启点击的收尾"，予以忽略。
// 一旦用户重新按过（无论内外），后续的 UP 都是真实点击，照常判定。
inline bool PopupUpIsOpeningRelease(bool downInList, bool downOutside, bool upInside) {
    // 开菜单之后重新按过 → 不是收尾，按真实点击处理。
    if (downInList || downOutside) return false;
    // 没按过、且松开在列表外 → 就是开菜单那次点击的收尾（锚点在列表外）。
    return !upInside;
}

// 平铺列表：模态，选中返回行索引，取消返回 -1。screen 为屏幕坐标。
// minWidthPx：最小宽度（物理像素，0 = 默认 160 逻辑像素）
// exactWidth：true 时宽度严格等于 minWidthPx（长路径用省略号），
//             用于地址栏右侧下拉——弹层必须与地址框左右边缘齐平。
// maxRows：最多一次显示多少行，超出才出滚动条（默认 20）。
//   标签右键菜单有 21 行（关闭族 6 + 编辑族 2 + 复制族 4 + 移动族 4 + 分隔/重开），
//   用默认 20 会平白多出一条滚动条 —— 右键菜单里出现滚动条是明显的坏味道。
//   只影响平铺列表的初始高度（树模式的 ResizeToFit 有自己的上限）。
// toggleKey：锚点键（见文件头）。同一锚点连点第二下 → 直接返回 -1（"取消"），
//   不创建窗口、不改任何状态。传 nullptr = 不做"再点即收"判定。
// init：初态（预选/滚到某行）。默认 -1 = 与以前完全一致。
// reorder：可选的行内拖拽重排通道（见 PopupReorder）。默认 = 不启用拖拽。
int ShowPopupList(HWND owner, const Theme& theme, POINT screen,
                  const std::vector<PopupRow>& rows, int minWidthPx = 0,
                  bool exactWidth = false, int maxRows = 20, const wchar_t* toggleKey = nullptr,
                  PopupInitial init = {}, PopupReorder reorder = {});

// 文件夹树：模态，返回选中节点的 path（空 = 取消）。screen 为屏幕坐标。
// 点击行主体 = 导航到该文件夹；点击右侧 chevron = 就地展开/收回子节点。
// toggleKey 同上：连点第二下直接返回空串（取消）。
std::wstring ShowFolderTree(HWND owner, const Theme& theme, POINT screen,
                            std::vector<TreeNode> topNodes, TreeChildrenFn children,
                            int minWidthPx = 0, const wchar_t* toggleKey = nullptr);
