// 进程启动单测：重点是"不经 shell"这条性质。
//
// 【为什么这条性质要单测】它是 dev.* 全部命令的安全底线。
// 以前 dev.build 的命令行是拼出来的（`python build_fast.py && <target>`），
// 一个 `&& calc` 就是任意执行；改成 CreateProcess + 参数数组之后，
// 参数在物理上就**不是命令**了 —— 这个性质必须被测住，
// 否则将来有人"图方便"改回拼串，没人会发现。
#include <cstdio>
#include <string>
#include <vector>

#include "dev_proc.h"
#include "str.h"

static int pass = 0, fail = 0;
static void Expect(bool c, const char* what) {
    if (c) ++pass;
    else { ++fail; std::printf("  FAIL  %s\n", what); }
}
static void ExpectEq(const std::wstring& got, const wchar_t* want, const char* what) {
    if (got == want) ++pass;
    else {
        ++fail;
        std::printf("  FAIL  %s\n        期望：%ls\n        实际：%ls\n", what, want, got.c_str());
    }
}

int main() {
    std::printf("进程启动（不经 shell）\n");

    // ---- 转义规则（Win32 CommandLineToArgvW）----
    ExpectEq(devproc::QuoteArg(L"abc"), L"abc", "无特殊字符不加引号");
    ExpectEq(devproc::QuoteArg(L""), L"\"\"", "空串要加引号");
    ExpectEq(devproc::QuoteArg(L"a b"), L"\"a b\"", "含空格加引号");
    ExpectEq(devproc::QuoteArg(L"a\"b"), L"\"a\\\"b\"", "内部引号被转义");
    // 尾随反斜杠：**只在需要加引号时**才翻倍。
    // 不加引号的实参里反斜杠是字面量（argv 还原出来还是它自己）；
    // 加了引号就必须翻倍，否则收尾引号会被吃掉。
    ExpectEq(devproc::QuoteArg(L"a\\"), L"a\\", "无引号时尾随反斜杠保持原样");
    ExpectEq(devproc::QuoteArg(L"a b\\"), L"\"a b\\\\\"", "有引号时尾随反斜杠翻倍");
    ExpectEq(devproc::QuoteArg(L"a&b"), L"a&b", "命令分隔符不是 shell 语法，无需引号");

    ExpectEq(devproc::BuildCommandLine(L"python", {L"build_fast.py"}),
             L"python build_fast.py", "单参数命令行");
    ExpectEq(devproc::BuildCommandLine(L"bash", {L"tests/run_all.sh", L"a b"}),
             L"bash tests/run_all.sh \"a b\"", "多参数命令行");

    // ---- 实跑：参数里的 shell 元字符必须原样到达程序 ----
    // 这条是整套改动的核心断言。若实现退回拼串 + cmd.exe，
    // 输出会变成 "&&" 之前的内容，后面被 shell 执行掉了。
    int code = -1;
    std::wstring out, err;
    const bool launched = devproc::RunCapture(
        L"python", {L"-c", L"print('A&&calc B')"}, L"", &code, &out, &err);
    if (!launched) {
        std::printf("  SKIP  找不到 python，跳过实跑断言（%ls）\n", err.c_str());
    } else {
        Expect(code == 0, "命令正常退出");
        const std::wstring trimmed = str::Trim(out);
        Expect(trimmed == L"A&&calc B", "shell 元字符原样到达程序，没有被解释");
    }

    // ---- 退出码要如实带回来 ----
    if (launched) {
        int rc = -1;
        std::wstring out2, err2;
        devproc::RunCapture(L"python", {L"-c", L"import sys; sys.exit(3)"}, L"", &rc, &out2, &err2);
        Expect(rc == 3, "非零退出码如实返回（不是一律 0）");
    }

    std::printf("通过 %d / 失败 %d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}

