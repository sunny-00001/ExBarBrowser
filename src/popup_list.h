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
#pragma once
#include "common.h"
#include "theme.h"

struct PopupRow {
    std::wstring label;
    std::wstring path;
    HICON icon = nullptr;   // 调用方拥有；弹层不销毁
    bool folder = false;    // 行右端画 chevron（表示还有下一级）
    bool separator = false;
    bool disabled = false;
    int data = -1;
};

// 文件夹树节点（对应 Exbar.cpp 的 TreeNode）。
// 顶层节点由调用方构造并移交所有权；子节点由 below 的 children 回调懒加载创建；
// 整棵树（含子节点）的 icon 一律由弹层在结束时统一销毁。
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
};

// 懒加载回调：给定文件夹 path，返回其直接子文件夹节点。
// 只需填 label/path/icon/hasChildren；depth/parent/children 由弹层接管。
// 返回的 HICON 所有权移交给弹层。
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

// 平铺列表：模态，选中返回行索引，取消返回 -1。screen 为屏幕坐标。
// minWidthPx：最小宽度（物理像素，0 = 默认 160 逻辑像素）
// exactWidth：true 时宽度严格等于 minWidthPx（长路径用省略号），
//             用于地址栏右侧下拉——弹层必须与地址框左右边缘齐平。
int ShowPopupList(HWND owner, const Theme& theme, POINT screen,
                  const std::vector<PopupRow>& rows, int minWidthPx = 0,
                  bool exactWidth = false);

// 文件夹树：模态，返回选中节点的 path（空 = 取消）。screen 为屏幕坐标。
// 点击行主体 = 导航到该文件夹；点击右侧 chevron = 就地展开/收回子节点。
std::wstring ShowFolderTree(HWND owner, const Theme& theme, POINT screen,
                            std::vector<TreeNode> topNodes, TreeChildrenFn children,
                            int minWidthPx = 0);
