// 文件操作撤销栈的实现（纯策略，无 COM）。
//
// 机械动作（改名/移动/复制/删除）由调用方注入 Applier —— 见 file_undo.h 的说明。
#include "file_undo.h"

namespace fileundo {

void Stack::Push(Entry e) {
    if (e.items.empty()) return;      // 空记录撤不了，也不该占位置
    entries_.push_back(std::move(e));
    // 上限与 bmk.* 一致（20 步）。超出的从**底部**丢 —— 栈是 LIFO，
    // 底部是最老的一步，也是最不容易马上要撤的一步。
    while (entries_.size() > kMax) entries_.erase(entries_.begin());
}

bool Stack::TopFromMachine() const {
    if (entries_.empty()) return false;
    return entries_.back().byMachine;
}

const Entry* Stack::Top() const {
    if (entries_.empty()) return nullptr;
    return &entries_.back();
}

bool Stack::Undo(bool machineCaller, const Applier& apply, std::wstring* why) {
    if (entries_.empty()) {
        if (why) *why = L"没有可撤销的文件操作";
        return false;
    }
    Entry& e = entries_.back();
    // 边界规则：机器的撤销不穿透人工操作。
    if (machineCaller && !e.byMachine) {
        if (why) *why = L"栈顶是人工操作，机器的撤销不穿透人工操作";
        return false;
    }
    if (!apply) {
        if (why) *why = L"撤销执行器缺失";
        return false;
    }
    std::wstring reason;
    if (!apply(e, &reason)) {
        // 失败**不弹栈**：记录还在，人还能再试一次（也许是磁盘满了、也许是冲突）。
        // 应用器可以把已成功的项剔掉，所以重试只试剩下的。
        if (why) *why = reason.empty() ? L"撤销失败" : reason;
        return false;
    }
    entries_.pop_back();
    if (why) why->clear();
    return true;
}

}  // namespace fileundo
