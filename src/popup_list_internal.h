#pragma once
#include "popup_list.h"

// 内部头：只给 popup_list.cpp / popup_list_tree.cpp / popup_list_input.cpp 用。
// PopupState 是弹层状态机的私有实现（捕获、type-ahead、懒加载都在里面），
// 不对外暴露；拆成多 TU 只是让同一份状态有唯一定义，避免各文件抄一份字段。

struct PopupState {
    const std::vector<PopupRow>* rows = nullptr;   // 平铺模式数据
    const Theme* theme = nullptr;
    HWND self = nullptr;
    HWND list = nullptr;
    int hot = -1;
    bool done = false;
    bool wasActive = false;
    bool downInList = false;  // 必须先在本列表按下，抬起才算选中（否则开菜单那一下的抬起会立刻误触发）
    bool downOutside = false; // 按下点落在列表外（= 要收起）；等 UP 再关，见 WM_LBUTTONDOWN
    int picked = -1;
    int itemH = 24;
    int sepPx = 2;   // 分隔线只占 2px：它只是分组提示，不该和普通条目一样高
    int iconS = 16;
    bool above = false;  // true = 向上展开（底边锚定），树展开时不能盖住锚点控件
    // 有勾选行时弹层左侧多一列"勾选列"（Win32 / Fluent / ARIA 的做法）。
    // 整列要么都在要么都不在：只有一行可勾选也照样留整列，否则勾号会忽左忽右。
    bool checkCol = false;

    // 树模式（面包屑文件夹下拉）：对应 Exbar.cpp 的 PopupListState treeMode 分支
    bool treeMode = false;
    std::vector<TreeNode> tree;     // 所有节点（含懒加载出的子节点）
    std::vector<int> visible;       // 当前可见节点索引（显示顺序）
    TreeChildrenFn treeChildren;
    std::wstring treePicked;        // 树模式选中的 path（空=取消）
    int hotExpander = -1;           // 悬停在展开器上的 visible 行号
    int curSel = -1;                // 键盘选择（visible 行号）

    // 「再点即收」的锚点键（见 popup_toggle.h）。空 = 不参与判定。
    std::wstring toggleKey;

    // 初态：预选第几行 / 是否滚到它（见 popup_list.h 的 PopupInitial）。
    int selectRow = -1;
    bool scrollTo = true;
    int maxRows = 20;   // 可见行上限，预选居中滚动时要用

    // 键入首字母跳转（type-ahead）的累积缓冲。
    //
    // 为什么必须自己做：本列表是 LBS_OWNERDRAWVARIABLE 且**插入的是空字符串**
    // （文字在 WM_DRAWITEM 里按行号自绘），系统那套"键入字符 → LB_FINDSTRING"
    // 的默认行为完全失效 —— 它没有任何字符串可比。菜单 20+ 行时用户只能靠方向键挪，
    // 而 TC / Opus / 资源管理器的菜单都支持敲字母定位。
    std::wstring typeBuf;
    ULONGLONG typeTick = 0;
    // 连续按键超过这个间隔就当作"重新开始一个字"，否则累积成前缀。
    // 间隔不在这里写死：popupTypeAheadMs 是设置项（默认 900），慢打字者可调大。
    // 保留这个常量名只为注释可读，实际判定读 Config（见 WM_CHAR 分支）。

    // ---- 行内拖拽重排（可选通道，见 popup_list.h 的 PopupReorder）----
    PopupReorder reorder;
    // 弹层自持的行内容。宿主给了 rebuildRows 时由弹层持有（重排后要就地刷新）；
    // 否则 rows 指向调用方的只读数组，弹层不复制也不改。
    std::vector<PopupRow> rowsOwned;
    bool ownsRows = false;
    // 拖拽状态机：按下 → 移动超阈值 → 落点计算 → 松手提交。
    bool dragArm = false;      // 按下未超阈值
    bool dragging = false;     // 已超阈值，正在拖
    int dragFrom = -1;         // 起始行号
    int dropIndex = -1;        // 插入线位置（行号；-1 = 不画线）
    POINT dragStart{};
};

// 树 / 几何 / 命中：popup_list_tree.cpp 唯一实现，其它 TU 只调用。
void CollectAllVisible(PopupState* st);
void EnsureLoaded(PopupState* st, int ni);
int ItemFromPoint(HWND list, int x, int y);
int RowCount(const PopupState* st);
bool RowInert(const PopupState* st, int idx);
int RowHeight(const PopupState* st, int idx);
int TotalHeight(const PopupState* st, int n);
void RefreshHot(PopupState* st, HWND hList);
void ResizeToFit(PopupState* st);
void ToggleExpand(PopupState* st, int vi);

// 行内拖拽：落点提交（popup_list_input.cpp 唯一实现）。
// 提交后按 rebuildRows 就地刷新行内容（宿主重建 → 弹层重画）。
void PopupApplyReorder(PopupState* st, int from, int to);

// 输入状态机：popup_list_input.cpp 唯一实现，RunModal 负责装/卸。
LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);
