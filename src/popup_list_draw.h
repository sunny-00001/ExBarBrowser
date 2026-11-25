// 弹层的**行绘制**与**宽度计算**（从 popup_list.cpp 里拆出来的独立职责）。
//
// 【为什么拆】弹层窗口过程本身已经装了四件事：消息分发、鼠标捕获、
// type-ahead、树懒加载。行绘制是第五件，且是唯一一件"纯看数据画一画"的事 ——
// 它不碰窗口、不碰捕获、不改状态。混在一起时 popup_list.cpp 冲到 974 行，
// 再加任何绘制细节就必然超单文件 500 行闸门（AGENTS.md §3）。
//
// 【拆分的边界：绘制读状态，但不认识状态机】
// 行绘制需要知道"哪一行是 hover、哪一行是键盘选中、树里哪些节点可见"。
// 这些原本都住在 popup_list.cpp 的 PopupState 里。我们不把 PopupState 交出去
// （它是窗口过程的私有实现：捕获、type-ahead、懒加载都在里面），而是让调用方
// 递一份**只读快照**（PopupDrawParams）。这样以后状态机加字段不会顺带影响绘制，
// 绘制也不会偷偷改到状态机。
//
// 【共享规则：一个组件一份实现】
// 行右端的 chevron 命中区（PopupExpanderRect）同时被"画 chevron"和"判断点到
// 没点到 chevron"用到。这两处必须逐像素一致 —— 否则会出现"看得见但点不动"。
// 所以它放在头里 inline，两边共用，而不是复制两份。
#pragma once

#include "popup_list.h"

// 行绘制要用的状态快照（只读）。见文件头"拆分的边界"。
struct PopupDrawParams {
    const std::vector<PopupRow>* rows = nullptr;   // 平铺模式：行数据
    const std::vector<TreeNode>* tree = nullptr;     // 树模式：全部节点
    const std::vector<int>* visible = nullptr;        // 树模式：当前可见节点的树内索引
    const Theme* theme = nullptr;
    int iconS = 16;
    int hot = -1;          // hover 行（平铺=行号，树=visible 下标）
    int curSel = -1;       // 键盘选中行（同上口径）
    int hotExpander = -1;  // 悬停在 chevron 上的行
    bool treeMode = false;
    // 拖拽插入线：画在"插到第 dropIndex 行之前"的那一行上沿
    // （dropIndex == 行数时画在末行下沿）。-1 = 不画。
    //
    // 为什么由绘制层按行号自己判断画在哪一行：owner-draw 是**逐行**回调的，
    // 弹层没有"整块重画"的时机 —— 想画一条跨行的线，只能由那一行自己画。
    int dropIndex = -1;
    // 左侧勾选列：Win32 菜单 / Windows 11 Fluent / ARIA menuitemcheckbox 的做法 ——
    // 勾号占**最左边一整列**，不占文案。整列要么全在要么全不在
    // （见 HasCheckedRow）。这里只决定"画不画"，勾的是哪一行看 PopupRow::checked。
    bool checkCol = false;
};

// 画一行（owner-draw 的 WM_DRAWITEM）。
// 返回 TRUE = 已处理（Win32 的 WM_DRAWITEM 约定）。
bool PopupDrawItem(HDC dc, const DRAWITEMSTRUCT* dis, const PopupDrawParams& p);

// 弹层宽度：最宽文案 + 图标槽 + 勾选列 + 边距。
// exactWidth = true 时宽度严格等于 minWidthPx（长路径用省略号），
// 用于地址栏右侧下拉 —— 弹层必须与地址框左右边缘齐平。
int PopupMeasureWidth(HWND owner, const Theme& theme, const std::vector<PopupRow>* rows,
                      const std::vector<TreeNode>* tree, int iconS, bool exactWidth,
                      int minWidthPx);

// 行右端的 chevron 命中区（树模式）。
// 【为什么放头文件】画 chevron 的位置与"能被点中的区域"必须是同一个矩形。
// 两处共用这一个 inline，而不是复制两份 —— 复制的那份迟早和画的那份对不上，
// 于是出现"看得见但点不动"（或反之，"点得到却画不出来"）。
inline RECT PopupExpanderRect(const RECT& irc, const Theme& th) {
    const int pad = th.S(4);
    const int bw = th.S(18);
    RECT e;
    e.left = irc.right - pad - bw;
    e.top = irc.top;
    e.right = irc.right - pad;
    e.bottom = irc.bottom;
    return e;
}
