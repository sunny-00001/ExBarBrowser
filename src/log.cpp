// 诊断日志实现（唯一，架构规则 R7）。见 log.h。
#include "log.h"

#include "text_file.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// 日志文件名来自 identity.h（规则 B1：品牌串只允许出现在那里）。
// 显式包含而不是靠 common.h 传递 —— log.cpp 要能独立编译（单测就是这么编的）。
#include "identity.h"

namespace {

// 追加一行到 %TEMP%\ExBarBrowser.log（UTF-16 LE），并按大小轮转。
//
// 轮转逻辑（stat + 改名成 .1）与审计日志共用 tf::AppendLine；**编码不同是刻意的**
// —— 调试日志用 UTF-16 LE（Get-Content -Encoding Unicode 直接可读），
// 审计日志用 UTF-8（脚本 grep 方便）。所以这里只把编码结果与阈值传进去。
void AppendLine(const wchar_t* buf) {
    wchar_t dir[MAX_PATH] = L"";
    if (!GetTempPathW(MAX_PATH, dir)) return;
    const std::wstring path = std::wstring(dir) + id::kLogFileName;

    std::string bytes(reinterpret_cast<const char*>(buf), wcslen(buf) * sizeof(wchar_t));
    bytes.append("\r\n", 2);   // 与 WriteFile(L"\r\n", 4) 等价：2 个 wchar_t 的字节
    tf::AppendLine(path, bytes, 4ull << 20, 500);
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
