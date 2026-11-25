// 进程启动实现（接口见 dev_proc.h）。
#include "dev_proc.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

// 读线程与调用方共享的状态。
//
// 【为什么在堆上】读线程有可能在极端情况下 detach（见 RunCaptureTimeout 末尾）：
// detach 之后它仍在引用这些字段，而栈上的变量会随函数返回先失效 ——
// 线程再往里写就是野指针。堆上 + shared_ptr 才保证"线程活着，状态也活着"。
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    std::string bytes;
    bool overflow = false;
    bool done = false;
};

// 收尾窗口：主进程退出后，孙子可能还攥着管道的写端，读线程会一直等 EOF。
// 给它一点时间自己收工；到点由调用方动手（关作业对象 → 连带杀掉残留子孙）。
constexpr unsigned long long kDrainGraceMs = 2000;

// 收满输出之后的宽限：子进程卡在满管道上写不动，给它这么多时间自己收工。
// （旧实现里就有这个 10 秒，别在重写时丢掉它。）
constexpr unsigned long long kOverflowGraceMs = 10000;

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
                const std::wstring& cwd, int* code, std::wstring* output, std::wstring* err,
                bool* overflow) {
    return RunCaptureTimeout(program, args, cwd, 0, code, output, err, nullptr, overflow);
}

bool RunCaptureTimeout(const std::wstring& program, const std::vector<std::wstring>& args,
                       const std::wstring& cwd, unsigned long long timeoutMs,
                       int* code, std::wstring* output, std::wstring* err, bool* timedOut,
                       bool* overflow) {
    if (code) *code = -1;
    if (output) output->clear();
    if (timedOut) *timedOut = false;
    if (overflow) *overflow = false;

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

    // ---- 作业对象：进程树的天花板 ----
    //
    // 【为什么是作业对象而不是 TerminateProcess】只杀直接进程的话，孙子
    // （编译器、测试二进制、agent 自己起的守护进程）照样活着；而且它们
    // 攥着管道的写端不撒手，调用方连"读干输出"这一步都会永远卡住。
    // 作业对象 + KILL_ON_JOB_CLOSE 一次收掉整棵树。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
        ZeroMemory(&jl, sizeof(jl));
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl,
                                    sizeof(jl)) == FALSE) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    // 先冻住再归组：进程一启动就可能 fork 出孙子，那时再归组就追不上了
    // （孙子不会被自动收编）。CREATE_SUSPENDED 让"归组"发生在任何代码执行之前。
    const DWORD flags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                              flags, nullptr,
                              cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    // 【错误码必须立刻取】CloseHandle 也会写 last error，
    // 在关句柄之后再看 GetLastError，拿到的可能是关闭操作的错误，
    // 而不是"为什么没启动"。报一个错的错误码，比不报更坏 ——
    // 调用方会照着它去查一个根本没发生的问题。
    const DWORD createErr = ok ? ERROR_SUCCESS : GetLastError();
    // 我们的写端副本必须马上关掉，否则读端永远等不到 EOF。
    CloseHandle(writePipe);
    if (nul) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readPipe);
        if (job) CloseHandle(job);
        if (err) {
            *err = L"无法启动 " + program + L"（错误 " + std::to_wstring(createErr) + L"）";
        }
        return false;
    }

    if ((flags & CREATE_SUSPENDED) != 0) {
        if (job && AssignProcessToJobObject(job, pi.hProcess) == FALSE) {
            // 归不进作业对象（宿主自己已经在作业里时会发生）→ 降级：
            // 只剩"杀直接进程"的能力，但**不能卡住**，所以照常解冻。
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);   // 冻结的进程必须显式解冻，否则它永远不会跑
    }

    // ---- 读输出：放到独立线程 ----
    //
    // 【为什么不能就地读】旧实现在主线程里 Drain（阻塞到 EOF），**然后**才去
    // 等进程。于是"进程不退、管道不 EOF"这一种情况会卡在读上，超时根本轮不到 ——
    // 这正是 P1-7 说的"超时只是淘汰不是杀死"的根因：等待本身没有时限。
    // 现在读与等分开：读线程负责收输出，主线程负责盯时限并负责杀。
    auto shared = std::make_shared<Shared>();
    std::thread reader([shared, readPipe]() {
        std::string local;
        bool localOverflow = false;
        Drain(readPipe, &local, &localOverflow);
        // 收工就断开管道：子进程继续写会拿到 broken pipe，而不是把自己卡死。
        CloseHandle(readPipe);
        {
            std::lock_guard<std::mutex> lk(shared->mu);
            shared->bytes = std::move(local);
            shared->overflow = localOverflow;
            shared->done = true;
        }
        shared->cv.notify_all();
    });

    const ULONGLONG t0 = GetTickCount64();
    bool killedByTimeout = false;
    ULONGLONG overflowAt = 0;   // 收满输出的时刻（0 = 还没收满）
    for (;;) {
        const DWORD r = WaitForSingleObject(pi.hProcess, 50);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;   // 主进程退了
        const ULONGLONG now = GetTickCount64();
        if (overflowAt == 0) {
            std::lock_guard<std::mutex> lk(shared->mu);
            if (shared->done && shared->overflow) overflowAt = now;
        }
        // 硬超时：调用方给的时限。这是**真的会杀**的那道闸。
        if (timeoutMs > 0 && now - t0 >= timeoutMs) {
            killedByTimeout = true;
            break;
        }
        // 收满之后的宽限：子进程卡在满管道上写不动，给它 10 秒自己收工。
        if (overflowAt != 0 && now - overflowAt >= kOverflowGraceMs) {
            killedByTimeout = true;
            break;
        }
    }

    if (killedByTimeout) {
        // 杀整棵进程树：只杀直接进程的话，编译器/测试二进制这些孙子还活着，
        // 而且它们攥着管道写端不放 → 读线程永远等不到 EOF。
        if (job) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 2000);
        if (timedOut) *timedOut = true;
    }

    // 主进程退了以后，孙子可能还攥着管道写端 → 读线程还在等 EOF。
    // 给它一个收尾窗口；窗口用尽就关掉作业对象
    // （KILL_ON_JOB_CLOSE 连带杀掉残留子孙）→ 管道随之 EOF，读线程收工。
    bool readerFinished = false;
    {
        std::unique_lock<std::mutex> lk(shared->mu);
        readerFinished = shared->cv.wait_for(
            lk, std::chrono::milliseconds(kDrainGraceMs), [&] { return shared->done; });
    }
    if (!readerFinished && job) {
        CloseHandle(job);
        job = nullptr;
        std::unique_lock<std::mutex> lk(shared->mu);
        readerFinished = shared->cv.wait_for(
            lk, std::chrono::milliseconds(kDrainGraceMs), [&] { return shared->done; });
    }
    if (reader.joinable()) {
        if (readerFinished) {
            reader.join();
        } else {
            // 极端情况：读线程赖着不走。状态在堆上（shared_ptr），detach 安全。
            reader.detach();
        }
    }
    if (job) CloseHandle(job);

    DWORD rc = 0;
    if (GetExitCodeProcess(pi.hProcess, &rc)) {
        if (code) *code = static_cast<int>(rc);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output) {
        std::lock_guard<std::mutex> lk(shared->mu);
        *output = str::FromUtf8(shared->bytes);
    }
    // 截断要如实报出去：被截掉的输出与"本来就这么长"**看起来完全一样**，
    // 上层分辨不出来 —— 对"拿来看全貌"的用途（diff、编译日志）就是假绿。
    if (overflow) {
        std::lock_guard<std::mutex> lk(shared->mu);
        *overflow = shared->overflow;
    }
    return true;
}

}  // namespace devproc
