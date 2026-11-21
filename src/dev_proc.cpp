// 进程启动实现（接口见 dev_proc.h）。
#include "dev_proc.h"

#include <windows.h>

#include <string>
#include <vector>

#include "str.h"

namespace devproc {
namespace {

// 读干一根管道。返回 false 表示管道读失败。
bool Drain(HANDLE pipe, std::string* bytes, bool* overflow) {
    char buf[8192];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(pipe, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr)) break;
        if (n == 0) break;
        if (bytes->size() + n > kMaxOutputBytes) {
            bytes->append(buf, kMaxOutputBytes - bytes->size());
            *overflow = true;
            return false;  // 收够了，让调用方去断开管道
        }
        bytes->append(buf, n);
    }
    return true;
}

}  // namespace

// Win32 的 CommandLineToArgvW 反解析规则（与 CRT 的 argv 解析一致）：
//   · 反斜杠只有**紧邻引号**时才有转义意义；
//   · 一串 n 个反斜杠后面跟着引号 → 2n 个反斜杠 + 一个转义引号；
//   · 一串 n 个反斜杠后面**没有**引号 → 原样保留 n 个；
//   · 参数整体被引号包住时，末尾那串反斜杠要翻倍（否则会把收尾引号吃掉）。
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;  // 不需要引号
    }
    std::wstring out;
    out.push_back(L'"');
    size_t i = 0;
    while (i < arg.size()) {
        size_t slashes = 0;
        while (i + slashes < arg.size() && arg[i + slashes] == L'\\') ++slashes;
        if (i + slashes == arg.size()) {
            out.append(slashes * 2, L'\\');  // 尾随反斜杠：翻倍
            break;
        }
        if (arg[i + slashes] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(arg[i + slashes]);
        }
        i += slashes + 1;
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const std::wstring& program,
                               const std::vector<std::wstring>& args) {
    std::wstring cmd = QuoteArg(program);
    for (const std::wstring& a : args) {
        cmd.push_back(L' ');
        cmd += QuoteArg(a);
    }
    return cmd;
}

bool RunCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                const std::wstring& cwd, int* code, std::wstring* output, std::wstring* err) {
    if (code) *code = -1;
    if (output) output->clear();

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        if (err) *err = L"CreatePipe 失败";
        return false;
    }
    // 读端不继承：只有子进程需要写端。
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // 子进程的 stdin 接 NUL：不给它一个能读到东西的句柄，
    // 免得某个构建器等输入把我们挂住。
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;   // 同一根管道 → stdout 与 stderr 合流，顺序不保证但内容完整
    si.hStdInput = nul;

    std::wstring cmdLine = BuildCommandLine(program, args);
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr,
                              cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    // 我们的写端副本必须马上关掉，否则读端永远等不到 EOF。
    CloseHandle(writePipe);
    if (nul) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readPipe);
        if (err) {
            *err = L"无法启动 " + program + L"（错误 " + std::to_wstring(GetLastError()) + L"）";
        }
        return false;
    }

    std::string bytes;
    bool overflow = false;
    Drain(readPipe, &bytes, &overflow);
    // 关掉读端 = 断开管道。收够了（overflow）时这是**必须的**：
    // 否则子进程卡在满管道上写不动，我们又在等它退出 —— 死锁。
    CloseHandle(readPipe);

    // 收够了还赖着不走的（死循环写日志），10 秒后强杀 —— 绝不在这里无限等。
    const DWORD waitMs = overflow ? 10000 : INFINITE;
    if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD rc = 0;
    if (GetExitCodeProcess(pi.hProcess, &rc)) {
        if (code) *code = static_cast<int>(rc);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output) *output = str::FromUtf8(bytes);
    return true;
}

}  // namespace devproc
