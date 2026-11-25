// 临时调试工具：以调试器方式启动目标进程，在首次真实异常时写 minidump。
// 不注入、不发输入；只用于定位 ExBarBrowser 启动崩溃。
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

static bool ShouldDump(DWORD code) {
    return code == 0xC0000374 /*heap corruption*/ ||
           code == 0xC0000005 /*access violation*/ ||
           code == 0xC0000006 /*in-page error*/ ||
           code == 0xC0000025 /*noncontinuable exception*/ ||
           code == 0xC0000094 /*integer divide by zero*/ ||
           code == 0xC0000095 /*integer overflow*/ ||
           code == 0xC00000FD /*stack overflow*/ ||
           code == 0x80000003 /*breakpoint*/;
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    if (argc < 3) {
        ::fwprintf(stderr, L"usage: crash_capture <exe> <dump>\n");
        LocalFree(argv);
        return 2;
    }
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) { ::fwprintf(stderr, L"LoadLibrary dbghelp failed\n"); return 3; }
    auto pMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!pMiniDumpWriteDump) { ::fwprintf(stderr, L"GetProcAddress failed\n"); return 3; }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(argv[1], argv[1], nullptr, nullptr, FALSE,
                        DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr,
                        &si, &pi)) {
        ::fwprintf(stderr, L"CreateProcess failed %lu\n", GetLastError());
        return 4;
    }

    bool dumped = false;
    int exc_index = 0;
    DEBUG_EVENT ev{};
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const EXCEPTION_DEBUG_INFO& ex = ev.u.Exception;
            const DWORD code = ex.ExceptionRecord.ExceptionCode;
            const void* addr = ex.ExceptionRecord.ExceptionAddress;
            ++exc_index;
            ::fwprintf(stderr, L"[exc %d] tid=%lu code=0x%08X addr=%p firstChance=%d\n",
                       exc_index, ev.dwThreadId, code, addr,
                       (int)ex.dwFirstChance);
            ::fflush(stderr);

            // 跳过加载器断点（第一次 0x80000003），它不算崩溃。
            const bool loader_bp = (exc_index == 1 && code == 0x80000003);
            if (!dumped && !loader_bp && ShouldDump(code)) {
                HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                           FALSE, ev.dwThreadId);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (th && GetThreadContext(th, &ctx)) {
                        // MiniDumpWriteDump 要求 ExceptionPointers 指向调用方地址空间内的
                        // EXCEPTION_POINTERS（ClientPointers=FALSE），不能直接把调试事件里的
                        // EXCEPTION_RECORD 指针塞进去。
                        EXCEPTION_RECORD er = ex.ExceptionRecord;
                        EXCEPTION_POINTERS ep{};
                        ep.ExceptionRecord = &er;
                        ep.ContextRecord = &ctx;
                        MINIDUMP_EXCEPTION_INFORMATION mei{};
                        mei.ThreadId = ev.dwThreadId;
                        mei.ExceptionPointers = &ep;
                        mei.ClientPointers = FALSE;
                        BOOL ok = pMiniDumpWriteDump(pi.hProcess, pi.dwProcessId, file,
                                                     MiniDumpWithFullMemory, &mei,
                                                     nullptr, nullptr);
                        ::fwprintf(stderr, L"[dump] code=0x%08X ok=%d\n", code, (int)ok);
                        dumped = true;
                    } else {
                        ::fwprintf(stderr, L"[dump] GetThreadContext failed %lu\n",
                                   GetLastError());
                    }
                    if (th) CloseHandle(th);
                    CloseHandle(file);
                } else {
                    ::fwprintf(stderr, L"[dump] CreateFile failed %lu\n", GetLastError());
                }
            }
            // 让进程按原始方式处理异常；崩溃类异常通常不可继续。
            cont = DBG_EXCEPTION_NOT_HANDLED;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) break;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FreeLibrary(dbg);
    LocalFree(argv);
    return dumped ? 0 : 5;
}
