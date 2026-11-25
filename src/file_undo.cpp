// 文件操作撤销栈的实现（纯策略，无 COM）。
//
// 机械动作（改名/移动/复制/删除）由调用方注入 Applier —— 见 file_undo.h 的说明。
#include "file_undo.h"

namespace fileundo {

void Stack::Push(Entry e) {
    if (e.items.empty()) return;      // 空记录撤不了，也不该占位置
    // 做了新操作 → 被撤销的那条链就此作废（标准 redo 语义）。
    // 不清的话 redo 会把用户已经放弃的旧状态又做回来。
    redo_.clear();
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
    // 【redo 副本必须在 apply 之前留】apply 成功会把 e.items 清空（表示全撤完），
    // 事后拷贝只能拿到空记录 —— 那就永远 redo 不回去了。
    const Entry snapshot = e;
    std::wstring reason;
    if (!apply(e, &reason)) {
        // 失败**不弹栈**：记录还在，人还能再试一次（也许是磁盘满了、也许是冲突）。
        // 应用器可以把已成功的项剔掉，所以重试只试剩下的。
        if (why) *why = reason.empty() ? L"撤销失败" : reason;
        return false;
    }
    entries_.pop_back();
    redo_.push_back(snapshot);
    if (redo_.size() > kMax) redo_.erase(redo_.begin());
    if (why) why->clear();
    return true;
}

// 一次撤到指定那一步（语义见 file_undo.h 的说明）。
size_t Stack::UndoTo(size_t index, bool machineCaller, const Applier& apply, std::wstring* why,
                     size_t* undone_out) {
    if (index >= entries_.size()) {
        if (why) *why = L"没有那么多步可撤";
        if (undone_out) *undone_out = 0;
        return 0;
    }
    size_t undone = 0;
    // index 含它自己：撤到第 index 步 = 撤 index+1 次。
    for (size_t i = 0; i <= index; ++i) {
        std::wstring reason;
        if (!Undo(machineCaller, apply, &reason)) {
            // 中途失败：**如实报撤了几步**，不把"撤了一半"包装成成功。
            // 已撤掉的那几步不会回到栈里（它们真的撤掉了）；
            // 失败的那一步留在栈顶（Undo 失败不弹栈），还能再试。
            if (why) {
                *why = reason.empty() ? (L"第 " + std::to_wstring(undone) + L" 步失败")
                                      : reason;
            }
            if (undone_out) *undone_out = undone;
            return undone;
        }
        ++undone;
    }
    if (why) why->clear();
    if (undone_out) *undone_out = undone;
    return undone;
}

bool Stack::Redo(bool machineCaller, const Applier& applyFwd, std::wstring* why) {
    if (redo_.empty()) {
        if (why) *why = L"没有可重做的文件操作";
        return false;
    }
    Entry& e = redo_.back();
    // 边界同 Undo：机器的重做不穿透人工操作。
    if (machineCaller && !e.byMachine) {
        if (why) *why = L"栈顶是人工操作，机器的重做不穿透人工操作";
        return false;
    }
    if (!applyFwd) {
        if (why) *why = L"重做执行器缺失";
        return false;
    }
    // 与 Undo 对称：apply 可能修改 items（剔掉已成功的），所以先留副本。
    const Entry snapshot = e;
    std::wstring reason;
    if (!applyFwd(e, &reason)) {
        if (why) *why = reason.empty() ? L"重做失败" : reason;
        return false;
    }
    redo_.pop_back();
    // 压回 undo 栈：重做也是一次"做过的操作"，应该还能再撤回去。
    // 保留原始 byMachine —— 撤/重做的来源标签不能在这一来一回里被洗掉。
    entries_.push_back(snapshot);
    if (entries_.size() > kMax) entries_.erase(entries_.begin());
    if (why) why->clear();
    return true;
}

}  // namespace fileundo
