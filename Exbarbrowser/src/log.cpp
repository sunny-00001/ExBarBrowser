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
    // 行尾必须是 UTF-16LE 的 "CR LF" 四个字节（两个 wchar_t）。
    // 之前只追加了 2 个 ASCII 字节 "\r\n" —— 在 UTF-16 文件里那是一个残缺字符，
    // 结果整个日志变成**一行**，grep/tail 都读不出条目（实测：日志解析只得到 1 行）。
    const wchar_t eol[2] = {L'\r', L'\n'};
    bytes.append(reinterpret_cast<const char*>(eol), sizeof(eol));
    tf::AppendLine(path, bytes, 4ull << 20, 500);
}

// 相对进程启动的毫秒数。启动链路（DPI -> COM -> CreateWindow -> Layout -> ShowWindow）
// 全靠日志排障，而**没有时间戳**时"窗口为什么 1 秒才出来"只能靠猜：
// 每段耗时要能对着数字读，否则"启动慢"永远是感觉而不是判据。
//
// 用 GetTickCount64 而不是 time()：它单调、毫秒级、无时区问题，且启动阶段
// 一定已可用。基准取第一次调用日志的瞬间 —— 日志的第一行是 0 ms，
// 之后的都是"距第一行多久"，与进程启动的绝对时刻无关（差值才是要看的东西）。
unsigned StartupMs() {
    static const ULONGLONG base = GetTickCount64();
    return static_cast<unsigned>(GetTickCount64() - base);
}

void LogV(const wchar_t* fmt, va_list ap) {
    wchar_t body[1024] = L"";
    _vsnwprintf(body, 1023, fmt, ap);
    // 前缀 `[+  123 ms]` 固定宽度：日志按行对比时各行的正文起点对齐，
    // 用 grep 前导零差异不会把行弄成锯齿。
    wchar_t buf[1088] = L"";
    _snwprintf(buf, 1087, L"[+%5u ms] %s", StartupMs(), body);
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
