// 文件操作撤销栈：记住"做过什么"，以便原路撤回。
//
// 【为什么必须自己建栈，而不是靠 shell 的撤销栈】
// 本工程所有落盘操作都带 FOF_ALLOWUNDO，理论上它们进了系统撤销栈
// （资源管理器 Ctrl+Z 撤的就是它）。但取系统撤销谓词只能向**视图的背景
// IContextMenu** 要，而宿主 IExplorerBrowser 的背景菜单里**没有** undo 行
// （2026-11-21 实测：删除/改名之后 file.undo 恒失败，撤销无效）。
//
// 于是有两条路：① 宣称"可撤销"却兑现不了（那就是在骗 AI）；② 自己记一份
// "做过什么"，撤的时候按记录的逆操作做回去。这里选 ②。
//
// 【记什么：逆操作，不是快照】
// 书签那种"整棵树快照"的玩法在文件上不成立 —— 一个几百 MB 的目录没法快照。
// 这里记的是**能反向执行的那一步**：
//   改名 → 记旧名与新名，撤销 = 改回旧名；
//   移动 → 记原路径与新路径，撤销 = 移回原目录；
//   复制 → 记副本路径，撤销 = 删掉副本。
// 代价是"撤销"本身也是一次文件操作（同样可能失败）—— 所以撤销失败时
// **保留记录**，不把撤销机会弄丢。
//
// 【边界规则（同 bmk.*）：机器的撤销不穿透人工操作】
// AI 一次批量 undo 不能把用户几天前的手工整理全部滚掉
// （2026-11-21 结构化审计实测：40 次 undo 穿透回滚了全部历史）。
// 所以每条记录都带 byMachine 标签：机器发起撤销时，若栈顶是人工操作则拒绝。
//
// 本文件**不含任何 COM 调用** —— 纯策略与状态，可单测；
// 真正动文件的部分由调用方以 Applier 回调注入（见 Stack::Undo）。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace fileundo {

// 一次可撤销的文件操作。
enum class Op {
    Rename,   // 改名：撤销 = 改回旧名
    Move,     // 移动：撤销 = 移回原目录
    Copy,     // 复制：撤销 = 删掉副本
};

// 记录里的一项（一次命令可能影响多个文件）。
struct Item {
    std::wstring beforePath;   // 操作前的完整路径
    std::wstring afterPath;    // 操作后的完整路径（改名/移动/复制后的落点）
    std::wstring beforeName;   // 操作前的名字（改名撤销时直接用）
};

// 一条记录 = 一次命令调用 = 一步撤销。
struct Entry {
    Op op = Op::Rename;
    std::vector<Item> items;
    bool byMachine = false;    // 产生这条的操作是不是机器（AI）发的
    std::wstring label;        // 人看的描述（审计与日志）
};

// 应用一条记录（真正动文件）。返回 false = 这一步没撤成（原因写进 why）。
// 由调用方注入 —— 本模块不认识 COM，也不认识窗口。
//
// 记录以**非 const 引用**传入：撤销可能只成功一部分（多文件操作里很常见），
// 应用器可以把"已经撤成功"的项从记录里剔掉，让下一次重试只试剩下的 ——
// 否则已撤掉的那几项会一直失败，把这条记录永久卡死。
using Applier = std::function<bool(Entry&, std::wstring* why)>;

// 文件操作撤销栈（LIFO，上限 20 —— 同 bmk.* 的口径）。
class Stack {
public:
    static constexpr size_t kMax = 20;

    void Push(Entry e);
    bool CanUndo() const { return !entries_.empty(); }
    size_t Size() const { return entries_.size(); }
    // 栈顶是不是机器产生的。
    bool TopFromMachine() const;
    // 取栈顶（不弹出）。空栈返回 nullptr。
    const Entry* Top() const;

    // 撤销栈顶一步。
    //   · machineCaller=true 且栈顶是**人工**操作 → 拒绝（返回 false，不动栈）；
    //   · 执行失败 → 返回 false，且**保留**记录（撤销机会不能被一次失败吃掉）。
    bool Undo(bool machineCaller, const Applier& apply, std::wstring* why);

    void Clear() { entries_.clear(); }

private:
    std::vector<Entry> entries_;
};

}  // namespace fileundo
