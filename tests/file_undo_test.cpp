// 文件操作撤销栈的自测（src/file_undo.h）。
//
// 测的是**策略**（不碰 COM、不碰文件系统）：
//   · LIFO 顺序
//   · 上限 20（超出的从底部丢）
//   · 边界规则：机器的撤销不穿透人工操作
//   · 撤销失败**不弹栈**（记录还在，还能再试）
//   · 部分失败时应用器可以剔掉已成功的项（否则记录会被卡死）
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc tests/file_undo_test.cpp \
//     src/file_undo.cpp -o /tmp/file_undo_test.exe -static-libstdc++ -static-libgcc
#include <cstdio>
#include <string>

#include "file_undo.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

fileundo::Entry MakeEntry(fileundo::Op op, bool byMachine, const wchar_t* before,
                          const wchar_t* after) {
    fileundo::Entry e;
    e.op = op;
    e.byMachine = byMachine;
    fileundo::Item item;
    item.beforePath = before;
    item.afterPath = after;
    item.beforeName = L"a.txt";
    e.items.push_back(item);
    return e;
}

}  // namespace

int main() {
    std::printf("空栈不能撤\n");
    {
        fileundo::Stack s;
        Check(!s.CanUndo(), "空栈没有可撤的");
        Check(s.Top() == nullptr, "空栈没有栈顶");
        std::wstring why;
        Check(!s.Undo(false, [](fileundo::Entry&, std::wstring*) { return true; }, &why),
              "空栈撤销返回失败");
        Check(!why.empty(), "给出人可读的原因");
    }

    std::printf("\nLIFO：后进先出\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\a.txt", L"C:\\b.txt"));
        s.Push(MakeEntry(fileundo::Op::Move, false, L"C:\\b.txt", L"D:\\b.txt"));
        Check(s.Size() == 2, "栈里有 2 条");
        Check(s.Top() && s.Top()->op == fileundo::Op::Move, "栈顶是后进的那条");

        std::wstring why;
        int seen = 0;
        fileundo::Op order[2] = {fileundo::Op::Move, fileundo::Op::Rename};
        bool ok = true;
        while (s.CanUndo() &&
               s.Undo(false, [&](fileundo::Entry& e, std::wstring*) {
                   if (seen < 2 && e.op != order[seen]) ok = false;
                   ++seen;
                   return true;
               }, &why)) {
        }
        Check(ok, "撤销顺序是后进先出");
        Check(seen == 2, "两步都撤了");
        Check(!s.CanUndo(), "栈空了");
    }

    std::printf("\n上限 20：多出来的从底部丢\n");
    {
        fileundo::Stack s;
        for (int i = 0; i < 25; ++i)
            s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\a.txt", L"C:\\b.txt"));
        Check(s.Size() == fileundo::Stack::kMax, "栈长度被截到上限");
    }

    std::printf("\n边界规则：机器的撤销不穿透人工操作\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, /*byMachine=*/false, L"C:\\a.txt", L"C:\\b.txt"));
        int applied = 0;
        std::wstring why;
        Check(!s.Undo(/*machineCaller=*/true,
                      [&](fileundo::Entry&, std::wstring*) { ++applied; return true; }, &why),
              "机器撤人工的操作被拒绝");
        Check(applied == 0, "拒绝时没有真的去撤");
        Check(s.CanUndo(), "记录还在（没被吃掉）");
        Check(!why.empty(), "说明了原因");

        // 人工撤自己的 → 允许
        Check(s.Undo(false, [&](fileundo::Entry&, std::wstring*) { ++applied; return true; },
                      &why),
              "人工可以撤自己的操作");
        Check(applied == 1, "真的撤了");
    }

    std::printf("\n机器撤机器的 → 允许\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Move, /*byMachine=*/true, L"C:\\a.txt", L"D:\\a.txt"));
        int applied = 0;
        Check(s.Undo(true, [&](fileundo::Entry&, std::wstring*) { ++applied; return true; },
                      nullptr),
              "机器可以撤机器的操作");
        Check(applied == 1, "真的撤了");
    }

    std::printf("\n撤销失败不弹栈（记录还在，还能再试）\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\a.txt", L"C:\\b.txt"));
        std::wstring why;
        Check(!s.Undo(false, [](fileundo::Entry&, std::wstring* w) {
                  if (w) *w = L"文件被占用";
                  return false;
              }, &why),
              "执行失败 -> 撤销返回失败");
        Check(s.CanUndo(), "记录还在（一次失败不该吃掉撤销机会）");
        Check(why == L"文件被占用", "原因原样传给调用方");

        // 再试一次：这次成功 -> 记录才被弹掉
        Check(s.Undo(false, [](fileundo::Entry&, std::wstring*) { return true; }, nullptr),
              "重试成功");
        Check(!s.CanUndo(), "成功后记录被弹掉");
    }

    std::printf("\n部分失败：应用器剔掉已成功的项，记录不被卡死\n");
    {
        fileundo::Stack s;
        fileundo::Entry e;
        e.op = fileundo::Op::Move;
        e.byMachine = false;
        for (int i = 0; i < 3; ++i) {
            fileundo::Item item;
            item.beforePath = L"C:\\a.txt";
            item.afterPath = L"D:\\a.txt";
            item.beforeName = L"a.txt";
            e.items.push_back(item);
        }
        s.Push(e);
        int attempts = 0;
        std::wstring why;
        // 第一次：只撤成功第 1 项，其余失败（并把成功的剔掉）
        Check(!s.Undo(false, [&](fileundo::Entry& en, std::wstring* w) {
                  ++attempts;
                  // 模拟：第 1 项成功，其余失败
                  if (!en.items.empty()) en.items.erase(en.items.begin());
                  if (w) *w = L"部分失败";
                  return false;
              }, &why),
              "部分失败 -> 撤销返回失败");
        Check(s.CanUndo(), "记录还在");
        // 第二次：剩下的项都能撤 -> 成功并弹栈
        Check(s.Undo(false, [&](fileundo::Entry& en, std::wstring*) {
                  ++attempts;
                  en.items.clear();
                  return true;
              }, &why),
              "重试成功");
        Check(!s.CanUndo(), "记录被弹掉（没被卡死）");
        Check(attempts == 2, "一共试了两次");
    }

    std::printf("\n空记录不进栈（撤不了的东西不该占位置）\n");
    {
        fileundo::Stack s;
        fileundo::Entry e;
        e.op = fileundo::Op::Rename;
        e.items.clear();
        s.Push(e);
        Check(!s.CanUndo(), "空记录被忽略");
    }

    std::printf("\n空栈不能 redo\n");
    {
        fileundo::Stack s;
        Check(!s.CanRedo(), "空栈没有可重做的");
        std::wstring why;
        Check(!s.Redo(false, [](fileundo::Entry&, std::wstring*) { return true; }, &why),
              "空栈重做返回失败");
        Check(!why.empty(), "给出人可读的原因");
    }

    std::printf("\n撤销→重做→再撤销：记录来回弹栈，不丢也不重复\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\a.txt", L"C:\\b.txt"));
        std::wstring why;
        // 撤销成功 → redo 栈应有 1 条
        Check(s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
                  e.items.clear(); return true;
              }, &why), "撤销成功");
        Check(!s.CanUndo() && s.CanRedo(), "撤后 undo 空、redo 有 1 条");
        Check(s.RedoSize() == 1, "redo 栈长度正确");
        // 重做成功 → 记录回到 undo 栈
        Check(s.Redo(false, [](fileundo::Entry& e, std::wstring*) {
                  e.items.clear(); return true;
              }, &why), "重做成功");
        Check(s.CanUndo() && !s.CanRedo(), "重做后 undo 有 1 条、redo 空");
        Check(s.Top() && s.Top()->op == fileundo::Op::Rename, "回来的记录内容没变");
        // 还能再撤回去
        Check(s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
                  e.items.clear(); return true;
              }, &why), "重做后的记录还能再撤");
        Check(!s.CanUndo(), "再次撤销后 undo 空");
    }

    std::printf("\nredo 保留完整的 before/after（撤销清空 items 后仍能重做）\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Move, false, L"C:\\a.txt", L"D:\\a.txt"));
        std::wstring why;
        Check(s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
                  e.items.clear(); return true;  // 模拟真实执行器：成功后清空
              }, &why), "撤销清空 items");
        // 关键：redo 必须拿到撤销前的完整记录，而不是被清空的那份
        Check(s.Redo(false, [](fileundo::Entry& e, std::wstring*) {
                  if (e.items.empty() || e.items[0].beforePath != L"C:\\a.txt" ||
                      e.items[0].afterPath != L"D:\\a.txt")
                      return false;   // 记录丢了就不该声称重做成功
                  e.items.clear(); return true;
              }, &why), "重做拿到完整 before/after 并正向执行");
    }

    std::printf("\n新操作清空 redo 链（不能重做被放弃的旧状态）\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\a.txt", L"C:\\b.txt"));
        std::wstring why;
        s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
            e.items.clear(); return true;
        }, &why);
        Check(s.CanRedo(), "撤销后有可重做的");
        // 用户做了新操作
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:\\x.txt", L"C:\\y.txt"));
        Check(!s.CanRedo(), "新操作清空 redo 链");
    }

    std::printf("\nredo 边界：机器的重做不穿透人工操作\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, /*byMachine=*/false, L"C:\\a.txt", L"C:\\b.txt"));
        std::wstring why;
        // 人工撤销后，redo 栈顶是人工操作
        s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
            e.items.clear(); return true;
        }, &why);
        int applied = 0;
        Check(!s.Redo(/*machineCaller=*/true,
                      [&](fileundo::Entry&, std::wstring*) { ++applied; return true; }, &why),
              "机器重做人工操作被拒绝");
        Check(applied == 0 && s.CanRedo(), "拒绝时没真的做，记录还在");
    }

    std::printf("\nredo 失败不弹栈（能重试）\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Copy, false, L"C:\\a.txt", L"C:\\b.txt"));
        std::wstring why;
        s.Undo(false, [](fileundo::Entry& e, std::wstring*) {
            e.items.clear(); return true;
        }, &why);
        Check(!s.Redo(false, [](fileundo::Entry&, std::wstring* w) {
                  if (w) *w = L"目标被占用";
                  return false;
              }, &why), "重做失败返回失败");
        Check(s.CanRedo(), "失败后redo记录还在");
        Check(s.Redo(false, [](fileundo::Entry& e, std::wstring*) {
                  e.items.clear(); return true;
              }, &why), "重试成功");
        Check(!s.CanRedo(), "成功后 redo 栈空");
    }

    std::printf("\n步号口径（ByStepIndex）\n");
    {
        // 【为什么单测这个】对外的 index 是"从栈顶往回数"（0 = 下一步要撤的），
        // 而栈内部是正序（槽位 0 = 最早）。两个口径相反，混用会撤错步 ——
        // 而且命令照样返回成功，错得悄无声息。
        // 这里把映射钉死：index 0 必须是**最后 push** 的那条。
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////a.txt", L"C:////b.txt"));
        s.Push(MakeEntry(fileundo::Op::Move, false, L"C:////c.txt", L"D:////c.txt"));
        s.Push(MakeEntry(fileundo::Op::Copy, false, L"C:////e.txt", L"D:////e.txt"));
        const fileundo::Entry* top = s.ByStepIndex(0);
        Check(top != nullptr && top->op == fileundo::Op::Copy, "index 0 = 栈顶（最后 push 的那条）");
        const fileundo::Entry* mid = s.ByStepIndex(1);
        Check(mid != nullptr && mid->op == fileundo::Op::Move, "index 1 = 往回数第二条");
        const fileundo::Entry* old = s.ByStepIndex(2);
        Check(old != nullptr && old->op == fileundo::Op::Rename, "index 2 = 最早那一条");
        Check(s.ByStepIndex(3) == nullptr, "越界返回空（不是崩溃、也不是随便给一条）");
        Check(s.Size() == 3, "取记录不动栈");
        // redo 栈同口径：index 0 = 下一步要重做的那条。
        std::wstring why;
        size_t undone = 0;
        s.UndoTo(2, false, [](fileundo::Entry& e, std::wstring*) { e.items.clear(); return true; },
                 &why, &undone);
        const fileundo::Entry* r0 = s.RedoByStepIndex(0);
        Check(r0 != nullptr && r0->op == fileundo::Op::Rename, "redo index 0 = 第一个被撤的（最早那步）");
        Check(s.RedoByStepIndex(3) == nullptr, "redo 越界返回空");
    }

    std::printf("\n撤到指定步（UndoTo）\n");
    {
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////a.txt", L"C:////b.txt"));
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////c.txt", L"C:////d.txt"));
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////e.txt", L"C:////f.txt"));
        std::wstring why;
        size_t undone = 99;
        // index=1 = 撤到"栈顶往下第 2 条" = 连撤 2 次。
        const size_t got = s.UndoTo(1, false, [](fileundo::Entry& e, std::wstring*) {
            e.items.clear(); return true;
        }, &why, &undone);
        Check(got == 2 && undone == 2, "撤到第 1 步 = 撤掉 2 条");
        Check(s.Size() == 1, "栈里还剩 1 条");
        Check(s.RedoSize() == 2, "两步都进了 redo 栈");
    }
    {
        // 撤到越界：不许动栈，且要说清原因。
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////a.txt", L"C:////b.txt"));
        std::wstring why;
        size_t undone = 99;
        const size_t got = s.UndoTo(5, false, [](fileundo::Entry&, std::wstring*) { return true; },
                                    &why, &undone);
        Check(got == 0 && undone == 0, "越界一步都没撤");
        Check(s.Size() == 1, "越界不动栈");
        Check(!why.empty(), "越界给出人可读的原因");
    }
    {
        // 中途失败：已撤掉的**不回栈**，失败那步留在栈顶，还能再试。
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////a.txt", L"C:////b.txt"));
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////c.txt", L"C:////d.txt"));
        s.Push(MakeEntry(fileundo::Op::Rename, false, L"C:////e.txt", L"C:////f.txt"));
        std::wstring why;
        size_t undone = 99;
        int calls = 0;
        const size_t got = s.UndoTo(2, false, [&](fileundo::Entry& e, std::wstring* w) {
            ++calls;
            if (calls == 2) {           // 第二步失败
                if (w) *w = L"目标被占用";
                return false;
            }
            e.items.clear(); return true;
        }, &why, &undone);
        Check(got == 1 && undone == 1, "中途失败：如实报撤了 1 步");
        Check(!why.empty(), "中途失败给出原因");
        Check(s.Size() == 2, "已撤掉的那 1 条不回栈（栈剩 2 条）");
        Check(s.CanUndo(), "失败那步还在栈里（能再试）");
    }
    {
        // 机器撤到指定步：栈顶是人工操作时**整趟拒绝**（不穿透）。
        fileundo::Stack s;
        s.Push(MakeEntry(fileundo::Op::Rename, /*byMachine=*/false, L"C:////a.txt", L"C:////b.txt"));
        s.Push(MakeEntry(fileundo::Op::Rename, /*byMachine=*/false, L"C:////c.txt", L"C:////d.txt"));
        std::wstring why;
        size_t undone = 99;
        int applied = 0;
        const size_t got = s.UndoTo(1, /*machineCaller=*/true, [&](fileundo::Entry&, std::wstring*) {
            ++applied; return true;
        }, &why, &undone);
        Check(got == 0 && applied == 0, "机器撤人工操作被拒绝");
        Check(s.Size() == 2, "被拒绝时栈没动");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
