// 撤销 / 重做 两条命令族（file.undo / file.redo）与撤销历史的结构化出口。
//
// 单独成文件：撤销有自己的**两层语义**（应用内栈 → shell 的撤销栈），
// 里面每一段都有"为什么不能反过来"的推理；混在文件操作命令里，
// 读的人要翻过几百行才看到这段推理，而它恰恰是最容易写错的地方。
//
// 【口径】撤销栈在 src/file_undo.h；纯逻辑（撤到第 N 步、中途失败怎么算）
// 都在 Stack 里，那里可单测 —— 本文件只负责把结果如实地报出去。
#include "app.h"

#include "app_commands.h"
#include "shell_ops.h"

void App::RegisterUndoCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;
        // 撤销（Ctrl+Z）：两层，先撤自己的，再落 shell 的。
        //
        // 【为什么要有自己的一层】
        // shell 的撤销谓词只能向视图的背景 IContextMenu 要，而宿主 IExplorerBrowser
        // 的背景菜单里**没有** undo 行（2026-11-21 实测：删除/改名之后这条路恒失败）。
        // 于是我们自己的文件操作（改名/移动/复制）记一份"做过什么"，
        // 撤的时候按逆操作做回去（见 src/file_undo.h）。
        //
        // 【顺序不能反】
        // 先撤自己的栈；栈空（或撤不动）才落 shell 的撤销。
        // 反过来的话，shell 那个"永远撤不掉"的 undo 谓词会把我们自己的记录挡住，
        // 于是"能撤的也撤不掉"。
        //
        // 【边界】机器的撤销不穿透人工操作：栈顶是人工做的，机器撤不了
        //（同 bmk.* 的口径，见 file_undo.h）。
        reg(CmdId::CmdUndo, [this](const Args&) {
            const bool machine = CommandBus::Get().fromMachine();
            if (fileUndo_.CanUndo()) {
                std::wstring why;
                const bool ok = fileUndo_.Undo(
                    machine,
                    [this](fileundo::Entry& e, std::wstring* w) { return ApplyFileUndo(e, w); },
                    &why);
                if (ok) {
                    RefreshActive();
                    SyncUndoChip();   // 少了一步可撤：状态栏 chip 要跟着少
                    return true;
                }
                CommandBus::Get().NoteFailure(why.empty() ? L"撤销失败" : why);
                // 栈非空但撤不动 → **到此为止，不落回 shell**。
                //
                // 两种情形都必须停在这里：
                //   ① 栈顶是人工操作而调用方是机器 → 让机器去撤 shell 里的
                //      操作 = 穿透人工操作，这正是双门机制要防的。
                //   ② 撤销执行本身失败（文件被占用、权限不足）→ 那条记录**还在栈里**
                //      （失败不弹栈），此时落回 shell 会去撤**另一件不相干的事**：
                //      "撤 A 失败 → 居然撤了 B"。那比失败更糟 —— 用户以为退回了 A，
                //      实际丢的是 B。
                //
                // 只有"我们的栈本来就是空的"才轮到 shell 的撤销栈。
                return false;
            }
            // 自己没有可撤的 → 落 shell 的撤销栈（用户右键菜单做的操作在那里）。
            const bool ok = WithView([this](IShellView* v) {
                const bool r = shellops::Undo(v);
                if (r) RefreshActive();   // 撤销后的列表必须立刻反映出来
                return r;
            }, L"撤销");
            // 撤销走的是视图背景菜单里的 undo 谓词（不自建栈）。谓词不存在时
            // shellops::Undo 返回 false，调用方只看到"命令执行失败" —— 分不清是
            // "真的没有可撤的"还是"撤销执行失败"。这里至少把话说清楚。
            if (!ok) {
                CommandBus::Get().NoteFailure(
                    L"没有可撤销的操作（应用内撤销栈为空，系统的撤销栈里也没有可撤的项）");
            }
            return ok;
        });

        // 重做（Ctrl+Y）：只重做自己那一步栈。shell 没有可拿到的 redo 谓词
        //（与 undo 同样受 IExplorerBrowser 背景菜单限制，见 file_undo.h），
        // 所以这里不落 shell —— 没有可重做的就明说，不假装做了。
        // 边界同撤销：机器的重做不穿透人工操作（见 Stack::Redo）。
        // 撤销历史：栈里每条都带 label，列出来才知道"下一步会撤掉什么"。
        regR(CmdId::FileUndoList, [this](const Args&, json::Value* out) {
            // 【index 必须与 file.undo.to 同口径】file.undo.to 的 index 是
            // "从栈顶往回数"（0 = 下一步要撤的那条 = 撤 1 步）。
            // 而 entries_ 是**正序**（Push 走 push_back）：下标 0 = 最早那一步。
            // 以前这里按下标输出，等于告诉 AI "最早那一步是下一步要撤的" ——
            // AI 拿这个 index 去调 file.undo.to 会撤错步，而命令照样返回成功，
            // 错得悄无声息。
            // 现在编号一律由 Stack 的 ByStepIndex / RedoByStepIndex 提供
            // （口径只有一份，file_undo.h 里有说明），本函数只负责排版。
            auto pack = [this](bool redo) {
                json::Value arr = json::Value::MakeArray();
                const size_t n = redo ? fileUndo_.RedoSize() : fileUndo_.Size();
                for (size_t k = 0; k < n; ++k) {
                    const fileundo::Entry* e =
                        redo ? fileUndo_.RedoByStepIndex(k) : fileUndo_.ByStepIndex(k);
                    if (!e) break;
                    const wchar_t* op = e->op == fileundo::Op::Rename ? L"rename" :
                                       e->op == fileundo::Op::Move   ? L"move" : L"copy";
                    json::Value row = json::Value::MakeObject();
                    row.Set(L"index", json::Value::MakeInt(static_cast<int>(k)));
                    row.Set(L"label", json::Value::MakeStr(e->label));
                    row.Set(L"op", json::Value::MakeStr(op));
                    row.Set(L"machine", json::Value::MakeBool(e->byMachine));
                    row.Set(L"items", json::Value::MakeInt(static_cast<int>(e->items.size())));
                    arr.arr.push_back(std::move(row));
                }
                return arr;
            };
            out->Set(L"undo", pack(false));
            out->Set(L"redo", pack(true));
            out->Set(L"undoCount", json::Value::MakeInt(static_cast<int>(fileUndo_.Size())));
            out->Set(L"redoCount", json::Value::MakeInt(static_cast<int>(fileUndo_.RedoSize())));
            return true;
        });
        // 跳到指定步：0 = 栈顶，N = 连续撤 N+1 步。中途失败如实报告撤了几步，
        // 不把"撤了一半"包装成成功；已撤掉的那几步不会回到栈里。
        regR(CmdId::FileUndoTo, [this](const Args& args, json::Value* out) {
            const int index = args.Int(L"index", -1);
            if (index < 0 || static_cast<size_t>(index) >= fileUndo_.Size()) {
                CommandBus::Get().NoteFailure(L"撤销历史里没有第 " +
                                              std::to_wstring(index) + L" 步");
                return false;
            }
            const bool machine = CommandBus::Get().fromMachine();
            // 循环与"中途失败怎么算"都在 Stack::UndoTo 里（纯逻辑、可单测）——
            // 这里只负责把结果如实地报出去。
            std::wstring why;
            size_t undone = 0;
            fileUndo_.UndoTo(static_cast<size_t>(index), machine,
                            [this](fileundo::Entry& e, std::wstring* w) {
                                return ApplyFileUndo(e, w);
                            },
                            &why, &undone);
            const size_t want = static_cast<size_t>(index) + 1;
            const bool full = (undone == want);
            if (!full) {
                CommandBus::Get().NoteFailure(
                    L"只撤回了 " + std::to_wstring(undone) + L" 步，第 " +
                    std::to_wstring(undone) + L" 步失败：" +
                    (why.empty() ? L"原因未知" : why));
            }
            // 刷新：**撤了哪怕一步**都要刷（部分失败时已经有文件被改回去了）。
            // 只在完全成功才刷，会让"撤了一半"的视图停在旧状态。
            if (undone > 0) {
                RefreshActive();
                SyncUndoChip();   // 撤了几步就少几步
            }
            if (out) {
                out->Set(L"undone", json::Value::MakeInt(static_cast<int>(undone)));
                out->Set(L"remaining", json::Value::MakeInt(static_cast<int>(fileUndo_.Size())));
            }
            return full;
        });

        reg(CmdId::CmdRedo, [this](const Args&) {
            const bool machine = CommandBus::Get().fromMachine();
            if (!fileUndo_.CanRedo()) {
                CommandBus::Get().NoteFailure(L"没有可重做的文件操作");
                return false;
            }
            std::wstring why;
            const bool ok = fileUndo_.Redo(
                machine,
                [this](fileundo::Entry& e, std::wstring* w) { return ApplyFileRedo(e, w); },
                &why);
            if (ok) {
                RefreshActive();
                SyncUndoChip();   // 重做多回一步：可撤步数 +1
                return true;
            }
            CommandBus::Get().NoteFailure(why.empty() ? L"重做失败" : why);
            return false;
        });

        // 撤销历史弹层的打开入口（§2b B1）：chip 点击人独占 → 命令化。
        // 弹层里点行本来就走总线（file.undo.to / file.redo），
        // 这里补的只是"把它打开"这一步 —— 机器侧拿到的是可视化列表的入口。
        reg(CmdId::WinUndoHistory, [this](const Args&) {
            ShowUndoHistory();
            return true;
        });
}
