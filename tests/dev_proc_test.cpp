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

#include <windows.h>

#include "dev_proc.h"
#include "str.h"
#include "text_file.h"

namespace {

// 临时目录（与 tests/audit_test.cpp 同一做法：不引新依赖，AGENTS.md §1.2）。
std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return n ? std::wstring(buf, n) : std::wstring(L".");
}

// 把路径里的反斜杠换成正斜杠：python 的字符串里 'C:\Users' 会被当成转义序列，
// 正斜杠在 Windows 上一样能用，还省掉一层转义的坑。
std::wstring ToSlash(std::wstring p) {
    for (wchar_t& c : p) {
        if (c == L'\\') c = L'/';
    }
    return p;
}

}  // namespace

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

    // ---- 硬超时：到点真杀，而且杀的是**整棵进程树**（P1-7）----
    //
    // 【为什么必须测】"超时"如果只是报一句"超时了"而进程还在跑，
    // 那么 agent 交一份死循环的代码 = 开发机上多一个永远停不下来的进程。
    // 而"只杀直接进程"也不够：孙子（编译器、测试二进制）照样活着。
    // 这里同时钉两件事：① 到点就返回；② 孙子也一起死。
    if (launched) {
        // ---- ① 到点就返回 ----
        const ULONGLONG t0 = GetTickCount64();
        int rc = -1;
        std::wstring o3, e3;
        bool timedOut = false;
        const bool started = devproc::RunCaptureTimeout(
            L"python", {L"-c", L"import time; time.sleep(30)"}, L"", 1500, &rc, &o3, &e3,
            &timedOut);
        const unsigned long long spent = GetTickCount64() - t0;
        Expect(started, "超时用例：进程起来了");
        Expect(timedOut, "超时用例：timedOut=true（不是假超时）");
        Expect(spent < 15000, "超时用例：到点就返回，没等满 30 秒");

        // ---- ② 孙子也一起死 ----
        //
        // 父进程起一个孙子，孙子睡 3 秒后写一个标记文件。
        // 超时杀掉进程树之后，等 4 秒 —— 标记文件**不该**出现。
        // 它出现了，就说明"只杀了直接进程"，孙子还活着。
        const std::wstring dir = TempDir();
        const std::wstring marker = tf::Join(dir, L"ebb_devproc_timeout_marker.txt");
        const std::wstring script = tf::Join(dir, L"ebb_devproc_grandchild.py");
        DeleteFileW(marker.c_str());
        const std::wstring scriptPy = ToSlash(script);
        const std::wstring markerPy = ToSlash(marker);
        const std::wstring scriptBody =
            L"import time\n"
            L"time.sleep(3)\n"
            L"open(r'" + markerPy + L"','w').write('x')\n";
        if (!tf::WriteText(script, scriptBody)) {
            std::printf("  SKIP  写不了临时脚本，跳过进程树断言\n");
        } else {
            const std::wstring parentPy =
                L"import subprocess,sys,time\n"
                L"subprocess.Popen([sys.executable, r'" + scriptPy + L"'])\n"
                L"time.sleep(30)\n";
            int rc2 = -1;
            std::wstring o4, e4;
            bool timedOut2 = false;
            const bool started2 = devproc::RunCaptureTimeout(
                L"python", {L"-c", parentPy}, L"", 1500, &rc2, &o4, &e4, &timedOut2);
            Expect(started2, "进程树用例：父进程起来了");
            Expect(timedOut2, "进程树用例：父进程被超时掐断");
            // 孙子本来会在 3 秒后写标记；等它该写的时间过去再断言。
            Sleep(4000);
            Expect(!tf::Exists(marker), "进程树用例：孙子也被杀了（迟到 3 秒的标记文件没出现）");
            DeleteFileW(marker.c_str());
            DeleteFileW(script.c_str());
        }
    }

    std::printf("通过 %d / 失败 %d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}

