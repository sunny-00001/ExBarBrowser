// 参数对象化自测（第 1 步的判据）。
//
// 核心断言：**旧字符串入口与新对象入口必须落到同一结果**。
// 只要这条成立，改造就不改变既有行为（keymap / 序列 / 鼠标规则里写的是
// "ViewMode:3" 那种单值规格，它们必须继续工作）。
//
// 第二个断言：ArgsFromLegacyArg 从 argsSchema 推导键名 —— 这是删掉 kArgKeys
// 那张手工映射表的前提。如果推导错了，旧规格会静默失效。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/args_test.cpp src/commands.cpp src/json.cpp src/config.cpp src/text_file.cpp \
//     -o /tmp/args_test.exe -lole32 -luuid -lshell32 -ladvapi32
#include <cstdio>
#include <string>

#include "commands.h"

namespace {

int g_fail = 0;
Args g_last = Args::Empty();

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 解析 JSON 文本成 Args
Args From(const wchar_t* text) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(text, v, &err)) return Args::Empty();
    return Args::FromJson(v);
}

}  // namespace

int main() {
    std::printf("Args 基本访问\n");
    {
        Args a = From(L"{\"preset\":3,\"name\":\"x\",\"flag\":true}");
        Check(a.Int(L"preset") == 3, "Int 取值");
        Check(a.Str(L"name") == L"x", "Str 取值");
        Check(a.Bool(L"flag") == true, "Bool 取值");
        Check(a.Has(L"preset"), "Has 命中");
        Check(!a.Has(L"nope"), "Has 未命中");
        // 缺键返回默认值，不崩溃 —— 这是"拼错键名不会炸"的保证
        Check(a.Int(L"nope", -1) == -1, "缺键返回默认值");
        Check(a.Str(L"nope", L"d") == L"d", "缺键返回默认串");
    }

    std::printf("\n空参数\n");
    {
        Check(Args::Empty().IsEmpty(), "Empty() 是空");
        Check(From(L"{}").IsEmpty(), "{} 是空");
        Check(Args::Empty().Int(L"x") == 0, "空对象取 Int 得 0");
    }

    std::printf("\n从 schema 推导 legacy 参数键\n");
    {
        // ViewMode 的 schema: required=[preset], preset 是 integer
        const CommandDef* vm = FindCommand(L"ViewMode");
        Check(vm != nullptr, "找到 ViewMode");
        if (vm) {
            Args a = ArgsFromLegacyArg(*vm, L"3");
            Check(a.Int(L"preset") == 3, "\"ViewMode:3\" -> {preset:3}（整数）");
            Check(a.Int(L"preset", -1) == 3, "取用正确");
        }

        // ViewSort 的 schema: required=[by]
        const CommandDef* vs = FindCommand(L"ViewSort");
        if (vs) {
            Args a = ArgsFromLegacyArg(*vs, L"1");
            Check(a.Int(L"by") == 1, "\"ViewSort:1\" -> {by:1}");
        }

        // TabActivate: required=[index]
        const CommandDef* ta = FindCommand(L"TabActivate");
        if (ta) {
            Args a = ArgsFromLegacyArg(*ta, L"2");
            Check(a.Int(L"index") == 2, "\"TabActivate:2\" -> {index:2}");
        }

        // TabNew: path 是**非必填** string —— 推导应取 properties 第一个键
        const CommandDef* tn = FindCommand(L"TabNew");
        if (tn) {
            Args a = ArgsFromLegacyArg(*tn, L"D:\\tmp");
            Check(a.Str(L"path") == L"D:\\tmp", "\"TabNew:D:\\tmp\" -> {path:...}（字符串）");
        }
    }

    std::printf("\n无参数命令的 legacy 参数应被丢弃\n");
    {
        const CommandDef* nb = FindCommand(L"NavBack");
        if (nb) {
            Args a = ArgsFromLegacyArg(*nb, L"ignored");
            Check(a.IsEmpty(), "无参数命令不凭空造参数");
        }
    }

    std::printf("\n空字符串 -> 空参数（不构造假值）\n");
    {
        const CommandDef* vm = FindCommand(L"ViewMode");
        if (vm) {
            Check(ArgsFromLegacyArg(*vm, L"").IsEmpty(), "空串得到空对象");
        }
    }

    std::printf("\n新旧入口一致性（关键判据）\n");
    {
        // 用一个记录型 handler 验证：两条路径喂进去的 Args 必须等价
        CommandBus::Get().Register(CmdId::ViewMode, [](const Args& a) {
            g_last = a;
            return true;
        });

        CommandBus::Get().InvokeByNameLegacy(L"ViewMode:3", L"", Actor{ActorKind::Ai, L"test"});
        const int fromLegacy = g_last.Int(L"preset", -999);

        CommandBus::Get().InvokeByName(L"ViewMode", From(L"{\"preset\":3}"), Actor{ActorKind::Ai, L"test"});
        const int fromObject = g_last.Int(L"preset", -999);

        Check(fromLegacy == 3, "旧入口 \"ViewMode:3\" 得到 preset=3");
        Check(fromObject == 3, "新入口 {preset:3} 得到 preset=3");
        Check(fromLegacy == fromObject, "两条路径结果一致");
    }

    std::printf("\n审计描述不崩（参数摘要）\n");
    {
        // 只验证一轮调用能正常返回；审计内部会 Stringify 参数
        bool ok = CommandBus::Get().InvokeByName(L"ViewMode", From(L"{\"preset\":5}"), Actor{ActorKind::Ai, L"test"});
        Check(ok, "带审计的调用正常返回");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
