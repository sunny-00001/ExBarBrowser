// Shell 文件操作：复制/剪切/粘贴/删除/重命名/属性/打开/新建/选择/排序/分组/隐藏开关。
//
// 一律走 shell 自己的接口（IShellView / IFolderView2 / IContextMenu / IFileOperation），
// 不自己实现文件系统语义（回收站、冲突对话框、权限、撤销全部交给系统）。
#pragma once
#include <string>
#include <vector>

#include "common.h"

namespace shellops {

bool CopySelection(IShellView* view, bool cut);

// 把一组**指定路径**放进剪贴板（供随后的"粘贴"使用）。cut=true 时标记为移动。
//
// 与 CopySelection 的区别：那个抄的是"用户碰巧选中"的项，这个按路径点名。
// 机器没法表达"选中"，只能给路径 —— 这是它能表达"复制这几个"的唯一方式。
//
// 注意：这不是"产生副本"。写剪贴板的语义是"准备好，等粘贴"，与实际落盘是两件事。
// 想要真正复制到某处，用 FsCopyTo（一步到位）。
bool CopyPaths(const std::vector<std::wstring>& paths, bool cut);
bool Paste(IShellView* view, HWND owner);
bool DeleteSelection(IShellView* view, HWND owner);
bool InvokeSelectionVerb(IShellView* view, HWND owner, const char* verb);  // rename/properties/open
bool InvokeFolderVerb(IShellView* view, HWND owner, const char* verb);     // paste
bool NewFolder(IShellView* view, HWND owner);
bool NewTextFile(IShellView* view, HWND owner, std::wstring* created);
// 选中项恰好只有一个文件夹时返回其路径（用于"打开"文件夹 = 当前标签内进入）
bool FirstFolderSelection(IShellView* view, std::wstring* path);
// 把当前选中项复制/移动到 target 文件夹（IFileOperation，撤销与冲突交给系统）。
bool TransferToFolder(IShellView* view, HWND owner, const std::wstring& target, bool move);
bool SelectAll(IShellView* view);
bool SelectIndex(IShellView* view, int index);

// ---- 显式目标（业界 P2/P5：不依赖"用户碰巧选中了什么"）----
//
// 为什么需要这组：现有的 Copy/Delete/Rename 全部是 "Selection" 语义 —— 它们作用于
// 当前选中项。机器调用时不知道用户选了谁，暴露出去就会"AI 以为删了 A，实际删了 B"。
//
// 改法不是给每个操作写一份"按路径操作"的新实现（那会造成两份语义、迟早分叉），
// 而是**先按路径把目标选中，再复用既有的 Selection 实现**。
// 好处：文件语义（回收站、冲突对话框、权限、撤销）仍然只由一处负责。
//
// 代价：会改变用户的选中状态。这是可接受的 —— 命令本身有副作用，
// 且调用方既然点名了目标，选中集随之变化是符合直觉的。
// 清空选中（SelectNone）用于"这条命令不该留下副作用"之后恢复的场景。

// 清空当前选中。
bool SelectNone(IShellView* view);
// 按完整路径选中单个项目（不改变其他项之外的选中，会先清空）。
// 返回 false = 该项目不在当前视图里（可能已被删除/改名，或路径写错）。
bool SelectByPath(IShellView* view, const std::wstring& fullPath);

// 按一组完整路径选中（多选）。返回成功命中的个数。
//
// 为什么要这个：FsDelete / FsCopyTo 这类"批量对指定目标操作"的命令，
// 若在循环里"选一个做一次"，会 N 次触发视图刷新与 N 次 IFileOperation ——
// 慢、且中途失败会留下难解释的半完成状态。一次选中全部、一次操作，
// 系统的冲突/进度处理也就只有一轮。
int SelectByPaths(IShellView* view, const std::vector<std::wstring>& paths);
// 取得当前"唯一选中项"的完整路径。用于校验目标、以及把 Selection 语义
// 适配成"带目标"的调用（例如重命名要先知道改的是谁）。
bool SelectedSinglePath(IShellView* view, std::wstring* fullPath);

// 重命名指定项。newName 只给名字（不含路径）。带冲突检测。
// 与 InvokeSelectionVerb("rename") 的区别：那个会弹 shell 的输入框（交互式），
// 这个直接改（确定式，可被脚本/机器调用）。
bool RenameTo(IShellView* view, HWND owner, const std::wstring& fullPath,
              const std::wstring& newName);

// ---- 只读查询（业界 P6：读操作返回值）----
//
// 一律基于**当前视图**（IFolderView2）取数，而不是另扫盘：
//   · 与用户看到的完全一致（含虚拟命名空间、搜索结果、过滤状态）
//   · 不需要判断"这个路径能不能访问"，视图已经判断过了
// 代价是只能查"当前目录"，但这恰好是命令面板/AI 最需要的语义
// （"我现在看到的这些是什么"）。
struct DirEntry {
    std::wstring name;
    std::wstring path;      // 完整路径；命名空间项可能为空
    bool isFolder = false;
    unsigned long long size = 0;
    bool sizeKnown = false;
    bool hidden = false;
};
struct DirListing {
    std::wstring path;            // 被列出的目录
    std::vector<DirEntry> entries;
    bool truncated = false;       // 超过 limit 被截断
};
// limit <= 0 表示不限制。返回 false = 没有视图。
bool ListView(IShellView* view, int limit, DirListing* out);

// 单项目属性（按完整路径，必须在当前视图内）。
struct ItemInfo {
    std::wstring name;
    std::wstring path;
    bool isFolder = false;
    bool hidden = false;
    bool readonly_ = false;
    unsigned long long size = 0;
    bool sizeKnown = false;
    bool hasModified = false;
    SYSTEMTIME modified{};
    std::wstring typeName;   // "文本文档" / "文件夹" 这类
};
bool StatItem(IShellView* view, const std::wstring& fullPath, ItemInfo* out);
// ascending: 1=升序 0=降序 -1=沿用交互语义（对同一列再点一次就反向）
bool SetSort(IShellView* view, int mode, int ascending = -1);
// 当前视图的排序状态。返回 false 表示视图不支持。mode 含义同 SetSort；
// ascending 为当前实际方向。
bool GetSort(IShellView* view, int* mode, bool* ascending);
bool SetGroup(IShellView* view, int mode);  // 0 无 1 类型 2 日期 3 大小
bool ToggleHidden(IShellView* view, int which, HWND owner);  // which: 0 hidden 1 system 2 ext
int CycleHiddenState(IShellView* view, HWND owner);          // 返回新状态 0/1/2

}  // namespace shellops
