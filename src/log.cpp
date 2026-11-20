// 诊断日志实现（唯一，架构规则 R7）。见 log.h。
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

// 追加一行到 %TEMP%\ExBarBrowser.log（UTF-16 LE），并按大小轮转。
void AppendLine(const wchar_t* buf) {
    wchar_t path[MAX_PATH] = L"";
    if (!GetTempPathW(MAX_PATH, path)) return;
    lstrcatW(path, L"ExBarBrowser.log");

    // 轮转：每 500 行检查一次，超过 4 MB 重命名为 .1（只保留一份）。
    static int sinceCheck = 0;
    if (++sinceCheck >= 500) {
        sinceCheck = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 &&
            fad.nFileSizeLow > (4u << 20)) {
            const std::wstring old = std::wstring(path) + L".1";
            MoveFileExW(path, old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(wcslen(buf) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(h, L"\r\n", 4, &written, nullptr);
    CloseHandle(h);
}

void LogV(const wchar_t* fmt, va_list ap) {
    wchar_t buf[1024] = L"";
    _vsnwprintf(buf, 1023, fmt, ap);
    AppendLine(buf);
}

}  // namespace

void LogDbg(const wchar_t* fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LogLine(const wchar_t* fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}
