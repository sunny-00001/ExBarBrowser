// 文件夹树下拉的唯一入口（地址栏分隔符 / 书签图标共用）
//
// 为什么单独一个模块：点击地址栏分隔符与点击书签左侧图标，用户预期是**同一件事**
// ——"列出这一级的直接子文件夹，可就地逐级展开"。此前这段逻辑（枚举子文件夹 →
// 建 TreeNode → 调 ShowFolderTree）只存在于 nav_bar_actions.cpp 的匿名命名空间里，
// 书签栏要用只能复制一份；一旦两份各自演化，就会出现"地址栏能展开、书签栏只能
// 选一层"的观感分叉。数据源仍是 NavModel::ChildFolders（shell 枚举的唯一实现），
// 本文件只做「枚举结果 → TreeNode」与「锚点 → 弹出」两段胶水。
#pragma once
#include "common.h"
#include "popup_list.h"
#include "theme.h"

namespace folder_tree {

// 枚举 folder 的直接子文件夹并构造树节点。
// 返回节点的 icon 所有权随节点移交弹层（ShowFolderTree 结束统一销毁）。
// 不是文件夹 / 无权限 / 空目录 → 返回空（调用方按"没有子目录"处理）。
std::vector<TreeNode> Children(const std::wstring& folder);

// 在屏幕点 at 处展开文件夹树（at 一般取锚点控件底边左端），返回选中路径（空=取消）。
// top 为空时不弹窗，直接返回空 —— 与地址栏分隔符"没有子目录就不弹"同口径。
// 只做"建树 / 弹层"两段：空目录要不要退化成直接进入，由调用方按语境决定
// （书签栏进入该目录，面包屑什么都不做），本层不替调用方猜。
//
// toggleKey：「再点即收」的锚点键（见 popup_toggle.h）。同一个书签/
// 同一级面包屑必须传同一个串，否则"连点两下"会被判成两个不同锚点、开出两个菜单。
// 为空 = 不参与判定。
std::wstring Show(HWND owner, const Theme& theme, POINT at, std::vector<TreeNode> top,
                  int minWidthPx = 0, const wchar_t* toggleKey = nullptr);

}  // namespace folder_tree
