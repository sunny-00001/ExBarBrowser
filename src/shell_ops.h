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
// Shift+Delete：与 DeleteSelection 走同一份 IFileOperation，但不带 FOF_ALLOWUNDO，
// 即绕过回收站永久删除。仍保留 FOF_WANTNUKEWARNING 让 shell 弹最终警告。
bool DeleteSelectionPermanently(IShellView* view, HWND owner);
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
//
// why（非空时写入失败原因）：失败原因**只存在于 shell 的 HRESULT 里**，
// 不取出来就只能给一句"重命名失败"，而那句话对每种原因都不算错、
// 对使用者却毫无用处（换名？关掉占用程序？还是权限？）。
bool RenameTo(IShellView* view, HWND owner, const std::wstring& fullPath,
              const std::wstring& newName, std::wstring* why = nullptr);

// ---- 按路径的文件操作（撤销栈专用）----
//
// 见 src/shell_ops_path.h —— 那组按**完整路径**操作的文件操作已单独成文件：
// 它们的作用域（路径）与本文件其余部分（视图）不同，混在一起会让读的人
// 一路翻到文件末尾才发现还有另一套语义。

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

// ---- 按谓词选择（Ctrl+I 反选 / "只选文件" / "只选文件夹" / "按模式选"）----
//
// 【为什么在这里而不是散在命令处理器里】这四条命令的共同点是"读当前视图的全部
// 条目 → 用条件筛 → 一次选中"。若每条各写一遍，枚举/命中/刷新会分叉成四份：
// 反选忘了排除隐藏项、只选文件把文件夹也算上、按模式用错误的匹配语义……
// 而且 SelectByPaths 的"一次选中"是为了少触发刷新，四份实现很难都想到这一点。
//
// 谓词由调用方给（`const DirEntry&` -> bool），枚举与选中只有一份。
enum class SelectMode {
    Replace,   // 用命中集合替换当前选中（反选 / 只选文件 / 按模式）
    Add,       // 追加到当前选中（用户想"再加一批"）
};
//
// 返回命中并选中的条目数。0 = 没有条目满足条件（调用方应据此报 false，
// 而不是假装成功 —— 用户按 Ctrl+Shift+F 却什么都没被选中时必须能看出来）。
int SelectWhere(IShellView* view, const std::function<bool(const DirEntry&)>& pred,
                SelectMode mode = SelectMode::Replace);

// 按模式匹配名字（Ctrl+Shift+K）。用系统通配语义（`*` / `?`，大小写不敏感），
// 因为这是资源管理器/Total Commander 用户已经形成的肌肉记忆 —— 不要自创语法。
// 支持分号分隔多个模式（"*.txt;*.md"）；以 `.` 开头的模式按扩展名匹配（"*.txt" 等价）。
struct PatternOptions {
    bool matchFolders = false;   // 默认只匹配文件（"选图片"通常不想带上文件夹）
    bool caseSensitive = false;
};
int SelectByPattern(IShellView* view, const std::wstring& patterns,
                    const PatternOptions& opt = {});

// 当前选中项的路径列表（与视图顺序一致）。命令/UI 都需要"我选了什么"。
std::vector<std::wstring> SelectedPaths(IShellView* view);

// 撤销上一次文件操作（Ctrl+Z）。
//
// 【为什么不是自己维护一条撤销栈】
// 本工程所有落盘操作都走 IFileOperation 且带 FOF_ALLOWUNDO —— 它们**已经**进了
// 系统撤销栈（就是资源管理器里 Ctrl+Z 撤的那一条）。自己再记一份"做过什么"有两
// 个致命问题：
//   · 用户可能在资源管理器里也操作了同一批文件，两个栈的顺序会互相错位；
//   · 系统的撤销能处理"回收站还原 / 冲突改名回滚"这些细节，自建栈只能删/移回去。
// 正确做法是把 Ctrl+Z 转发给 shell 视图自己（它知道该撤什么）。
//
// 转发通道：给 shell 视图的宿主窗口发 WM_COMMAND，wParam 的高位是 EDCMD 通知码，
// 低位是 FCIDM 命令 id。DFM_INVOKECOMMAND + FCIDM_SHVIEWUNDO 就是"撤销"。
// 这是 shell 视图的标准协议（与 IExplorerBrowser 一致），不依赖内部窗口类名。
//
// 返回 false = 没有可撤销的操作，或视图没有响应（调用方应响一声而不是假装成功）。
// 注意：**没有 owner 参数**，是刻意的。
// 撤销动词偶尔会带出模态确认框；本函数会被 AI 通道在 UI 线程调用，
// 一旦进入模态循环就会卡死整个消息泵（实测：之后所有管道命令都返回空）。
// 撤销本身不需要人确认（它是恢复，不是破坏），所以一律以无 UI 方式调用。
bool Undo(IShellView* view);

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

// 读取当前的"隐藏项"显示设置。
//
// 为什么必须有读面：view.toggle.hidden / system / extensions 这三条命令
// 改的是 **进程级 shell 设置**（SHGetSetSettings），改完界面上立刻生效，
// 但状态快照里原来没有任何字段能反映它 —— 于是"隐藏文件开关到底有没有翻"
// 只能靠命令自述，而命令自述正是假绿的高发区。
//
// 读面与写面必须同源：这里用的是与 ToggleHidden/CycleHiddenState **同一个**
// HiddenFlags 掩码、同一份 SHELLSTATE。掩码若与写侧不一致，
// 就会出现"读到的不是刚改的那个开关"—— 那比没有读面更坏。
struct HiddenFlagsState {
    bool showAllObjects = true;   // 显示隐藏文件
    bool showSuperHidden = true;  // 显示系统文件
    bool showExtensions = true;   // 显示文件扩展名
    int triState = 0;             // 0 全显 / 1 隐系统 / 2 隐全部（与 CycleHiddenState 同口径）
};
HiddenFlagsState GetHiddenFlags();

// ---- 取接口（唯一实现）----
//
// `IShellView*` 拿到手之后，几乎每处都要 QI 一次 IFolderView2 / IShellFolder。
// 以前这个模板函数只在 shell_ops.cpp 的匿名命名空间里，外部拿不到，于是
// shell_view.cpp（3 处）与 view_stats.cpp（3 处）各自手写同一段 QI + 判空。
//
// 之所以对外暴露而不是各写一份：QI 有个容易写错的细节 ——
// `ComPtr::operator&` 会先 Reset 再返回地址，所以 `QueryInterface(..., &fv2)`
// 是安全的；但手写成 `reinterpret_cast<void**>(&fv2)` 时**不会** Reset，
// 若 fv2 非空就直接泄漏。把 QI 收进一个函数，这个坑只需要在一个地方处理。
//
// 失败返回空 ComPtr（不是 nullptr 裸指针），调用方用 `if (!fv2)` 判断即可。
ComPtr<IFolderView2> FolderView2(IShellView* view);
ComPtr<IShellFolder> ShellFolder(IFolderView2* fv2);

}  // namespace shellops
