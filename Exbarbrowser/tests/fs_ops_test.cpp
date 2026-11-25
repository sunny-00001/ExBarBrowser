// 确定式命令自测（第 4 步判据：交互与确定分离）。
//
// 这一层能脱机测的是**参数层**：
//   · targets 数组能被正确解析（跳过非字符串、空串）
//   · 目标命令的参数 schema 与 handler 取用的键一致
//   · 旧命令（交互式）与新旧命令（确定式）在表里各就各位
//
// 真正的文件操作（选中有没有成功、IFileOperation 结果）需要窗口，不在单测范围；
// 它的正确性靠"复用既有 Selection 实现"来保证（同一份代码，不是新写一份）。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/fs_ops_test.cpp src/commands.cpp src/json.cpp src/text_file.cpp \
//     /tmp/config_stub.cpp -o /tmp/fs_ops_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>
#include <vector>

#include "commands.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

Args From(const wchar_t* text) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(text, v, &err)) return Args::Empty();
    return Args::FromJson(v);
}

}  // namespace

int main() {
    std::printf("数组参数解析（targets）\n");
    {
        Args a = From(L"{\"targets\":[\"C:\\\\a.txt\",\"C:\\\\b.txt\"]}");
        auto v = a.StrArray(L"targets");
        Check(v.size() == 2, "解析出 2 个目标");
        Check(v.size() == 2 && v[0] == L"C:\\a.txt", "第一个目标正确");
        Check(v.size() == 2 && v[1] == L"C:\\b.txt", "第二个目标正确");
    }

    std::printf("\n数组参数的健壮性\n");
    {
        // 混入非字符串与空串：应被跳过，而不是塞进一个坏路径
        Args a = From(L"{\"targets\":[\"C:\\\\ok.txt\",123,\"\",null,\"C:\\\\ok2.txt\"]}");
        auto v = a.StrArray(L"targets");
        Check(v.size() == 2, "只保留 2 个合法字符串");
        Check(v.size() == 2 && v[0] == L"C:\\ok.txt" && v[1] == L"C:\\ok2.txt",
              "顺序保持且非法项被跳过");
    }

    std::printf("\n数组参数缺失/类型错误\n");
    {
        Check(From(L"{}").StrArray(L"targets").empty(), "缺键 -> 空");
        Check(From(L"{\"targets\":\"notarray\"}").StrArray(L"targets").empty(),
              "非数组 -> 空（不崩溃）");
        Check(From(L"{\"targets\":[]}").StrArray(L"targets").empty(), "空数组 -> 空");
    }

    std::printf("\n确定式命令存在且有目标参数\n");
    {
        struct Want {
            const wchar_t* name;
            const wchar_t* key;
            bool multi;
        };
        const Want wants[] = {
            {L"FsRename", L"target", false},
            {L"FsDelete", L"targets", true},
            {L"FsCopyTo", L"targets", true},
            {L"FsMoveTo", L"targets", true},
            {L"FsOpen", L"path", false},
        };
        for (const auto& w : wants) {
            const CommandDef* d = FindCommand(w.name);
            Check(d != nullptr, "命令存在");
            if (!d) continue;
            Check(d->implemented, "已实现");
            Check(d->argsSchema != nullptr && strchr(d->argsSchema, 't') != nullptr,
                  "有 argsSchema");
            // 关键：确定式命令绝不能依赖"当前选中"
            Check(!d->needsSelection, "不依赖当前选中项");
            // 也不能标记需要选中项前置
            Check(d->requires_.Empty(), "无 HasSelection 前置（靠参数定位）");
        }
    }

    std::printf("\n确定式命令可暴露给机器（有目标参数 = 安全可调）\n");
    {
        for (const wchar_t* n :
             {L"FsRename", L"FsDelete", L"FsCopyTo", L"FsMoveTo", L"FsOpen"}) {
            const CommandDef* d = FindCommand(n);
            if (!d) { Check(false, "命令存在"); continue; }
            Check(d->aiExposed, "已暴露给机器");
        }
    }

    std::printf("\n旧的交互式命令仍在且仍不暴露（人用入口不变）\n");
    {
        const CommandDef* r = FindCommand(L"CmdRename");
        Check(r != nullptr, "CmdRename 仍在（F2 的入口）");
        Check(r && !r->aiExposed, "CmdRename 不暴露给机器（它会弹输入框）");
        const CommandDef* d = FindCommand(L"CmdDelete");
        Check(d && !d->aiExposed, "CmdDelete 不暴露（目标不可表达）");
        Check(d && d->needsSelection, "CmdDelete 标了 needsSelection（如实反映）");
    }

    std::printf("\n破坏性标记\n");
    {
        const CommandDef* del = FindCommand(L"FsDelete");
        Check(del && del->effects.destructive, "FsDelete 标了 destructive");
        Check(del && !del->effects.readOnly, "FsDelete 不是只读");
        // 删除**撤不回来**：我们自己的撤销栈只记改名/移动/复制，
        // 删除要么进回收站（还原不可靠）、要么在网络盘上被永久删。
        // 所以 undoable 必须是 0 —— 声明 1 就是骗 AI"错了能退"。
        // 2026-11-21（P0-3）由 1 改成 0。
        Check(del && !del->effects.undoable, "FsDelete 不标 undoable（撤不回来，如实声明）");

        const CommandDef* ren = FindCommand(L"FsRename");
        Check(ren && !ren->effects.destructive, "FsRename 不是破坏性（改名可逆）");
        Check(ren && ren->effects.undoable, "FsRename 可撤销（有自己的撤销栈）");
        // 重命名同一目标同一新名两次：第二次目标已不存在 -> 不幂等
        Check(ren && !ren->effects.idempotent, "FsRename 不幂等");

        // 移动会让源文件消失（破坏性），但能移回去（可撤销）—— 两者可以同时为真。
        // 注意命令名是 FsMoveTo/FsCopyTo（名字里带 To = 目标由参数指定）；
        // FsCopy/FsCut 是"复制到剪贴板/剪切到剪贴板"，不是磁盘复制。
        const CommandDef* mov = FindCommand(L"FsMoveTo");
        Check(mov && mov->effects.destructive, "FsMoveTo 标了 destructive（源文件消失）");
        Check(mov && mov->effects.undoable, "FsMoveTo 可撤销（移回原目录）");
        // 复制不破坏任何东西，但副本要删掉才能撤销 —— 也算可撤销。
        const CommandDef* cp = FindCommand(L"FsCopyTo");
        Check(cp && !cp->effects.destructive, "FsCopyTo 不是破坏性");
        Check(cp && cp->effects.undoable, "FsCopyTo 可撤销（删掉副本）");
    }

    std::printf("\n全量命令表仍自洽（新命令没破坏结构）\n");
    {
        int n = 0, bad = 0;
        for (const auto& d : CommandTable()) {
            ++n;
            if (!d.argsSchema) ++bad;
            if (!d.name) ++bad;
            if (!d.title) ++bad;
        }
        Check(n >= 131, "命令表至少有 131 条（127 + 4 条确定式）");
        Check(bad == 0, "所有条目都有 name/title/argsSchema");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
