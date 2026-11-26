// 撤销历史弹层：把"做过什么"摊开，让人直接跳到某一步。
//
// 【为什么需要这个 UI】命令层早就有 file.undo.to{index}（一次撤到指定步），
// 但 index 是**给机器用的编号**：人不知道"第 3 步"对应哪次操作。
// 列表把每一步摊开（做了什么、对哪个文件），点哪一行就撤到那一步 ——
// 这也是 R20「人机等价」里 file.undo.to 那条人工路径：
// 命令面板能按 schema 追问 index，但那是"填数字"，不是"看清单挑一步"。
//
// 【数据源唯一】行内容直接取自 fileundo::Stack（fileUndo_），不另存一份：
// 另存就会与真实栈漂移（撤销/重做之后忘了更新，列表显示的是旧账）。
//
// 【口径对齐】列表的顺序与 file.undo.to 的 index 一致：index 0 = 栈顶
// （下一步要撤的那条），列表里也排在最上面。点第 k 行 = 撤 k+1 步。
#include "app.h"

#include "file_undo.h"
#include "menu.h"
#include "nav_path.h"

namespace {

// 动作号：撤销第 k 步 = kUndoBase + k；重做 = kRedo。
// 用大偏移（1000/2000）避开段号、菜单动作号那些小数字段。
constexpr int kUndoBase = 1000;
constexpr int kRedo = 2000;

std::wstring OpText(fileundo::Op op) {
    switch (op) {
    case fileundo::Op::Rename: return L"重命名";
    case fileundo::Op::Move:   return L"移动";
    case fileundo::Op::Copy:   return L"复制";
    }
    return L"操作";
}

// 一步的人类可读描述：单文件给"旧名 → 新名"，多文件给"共 N 项"。
//
// 【为什么不用 e.label】Entry::label 是命令级的短标签（"重命名" / "移动到"），
// 两个字看不出动的是谁 —— 列表的价值恰恰在于"看清单挑一步"。
std::wstring Describe(const fileundo::Entry& e) {
    if (e.items.size() == 1) {
        const auto& it = e.items.front();
        const std::wstring before = nav::Leaf(it.beforePath);
        const std::wstring after = nav::Leaf(it.afterPath);
        if (!before.empty() && !after.empty() && before != after)
            return before + L" → " + after;
        if (!after.empty()) return after;
        if (!before.empty()) return before;
    }
    if (e.items.size() > 1)
        return L"共 " + std::to_wstring(e.items.size()) + L" 项";
    return e.label;
}

}  // namespace

void App::ShowUndoHistory() {
    const auto& entries = fileUndo_.Entries();
    const auto& redo = fileUndo_.RedoEntries();
    if (entries.empty() && redo.empty()) {
        CommandBus::Get().NoteFailure(L"撤销历史是空的（没有可撤销的文件操作）");
        return;
    }

    ui::Menu menu;
    // 编号口径与 file.undo.to 完全一致（index 0 = 栈顶 = 下一步要撤的那条），
    // 取记录的入口也用同一个（Stack::ByStepIndex）—— 口径只有一份。
    const size_t n = entries.size();
    for (size_t index = 0; index < n; ++index) {
        const fileundo::Entry* e = fileUndo_.ByStepIndex(index);
        if (!e) break;
        menu.Add(L"撤销 " + std::to_wstring(index + 1) + L" 步 · " + OpText(e->op) + L" " +
                     Describe(*e),
                 kUndoBase + static_cast<int>(index), ico::IconId::Undo);
    }
    if (!redo.empty()) {
        if (!entries.empty()) menu.Separator();
        // index 0 = 下一步要重做的那条（重做只能一步步来）。
        const fileundo::Entry* e = fileUndo_.RedoByStepIndex(0);
        if (e)
            menu.Add(L"重做 · " + OpText(e->op) + L" " + Describe(*e), kRedo,
                     ico::IconId::Refresh);
    }

    // 锚点：状态栏的撤销 chip。弹层贴着它展开（chip 在屏幕底部，
    // 下方没有空间，弹层会自己翻到它上面）。
    RECT chip{};
    if (!statusBar_.SegmentScreenRect(Seg_Undo, &chip)) {
        // chip 不在场（状态栏被关掉）：退回主窗口左下角，
        // 别让弹层按一个无效坐标飞到屏幕外。
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        chip = {wr.left + 8, wr.bottom - 8, wr.right, wr.bottom};
    }
    POINT anchor{chip.left, chip.top};

    ui::MenuOptions opt;
    opt.toggleKey = L"status.undo";   // 再点即收（与状态栏右键菜单同一个锚点键族）
    opt.minWidthPx = theme_.S(320);
    const int action = menu.Show(hwnd_, theme_, anchor, opt);
    if (action < 0) return;

    // 派发一律走命令总线：审计、机器/人工的边界规则、失败上报只有这一条路。
    if (action == kRedo) {
        CommandBus::Get().Invoke(CmdId::CmdRedo, Args::Empty(), Actor::Ui());
        return;
    }
    if (action >= kUndoBase) {
        json::Value a = json::Value::MakeObject();
        a.Set(L"index", json::Value::MakeInt(action - kUndoBase));
        CommandBus::Get().Invoke(CmdId::FileUndoTo, Args::FromJson(a), Actor::Ui());
    }
}
