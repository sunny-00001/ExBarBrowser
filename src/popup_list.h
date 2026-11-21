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
};

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
int ShowPopupList(HWND owner, const Theme& theme, POINT screen,
                  const std::vector<PopupRow>& rows, int minWidthPx = 0,
                  bool exactWidth = false, int maxRows = 20, const wchar_t* toggleKey = nullptr,
                  PopupInitial init = {});

// 文件夹树：模态，返回选中节点的 path（空 = 取消）。screen 为屏幕坐标。
// 点击行主体 = 导航到该文件夹；点击右侧 chevron = 就地展开/收回子节点。
// toggleKey 同上：连点第二下直接返回空串（取消）。
std::wstring ShowFolderTree(HWND owner, const Theme& theme, POINT screen,
                            std::vector<TreeNode> topNodes, TreeChildrenFn children,
                            int minWidthPx = 0, const wchar_t* toggleKey = nullptr);
