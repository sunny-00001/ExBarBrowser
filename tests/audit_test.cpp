// 审计自测（E4：被拒绝的调用也必须留痕）。
//
// 【守的是什么】AI 通道有一整段在**进 CommandBus 之前**就返回了：
// 未知命令 / 未暴露 / 未实现 / schema 非法 / 参数不合法 / 危险未批准。
// 这些拒绝过去**一条审计都不写** —— 实测：连续 4 次被拒 → audit.log 0 增长。
//
// 【为什么这是真问题】被拒的调用恰恰是最值得追的：
//   · 反复撞同一个错 → 调用方（或模型）用错了方法；
//   · 反复撞 danger_disabled → 有人在试探破坏性操作的边界；
//   · 审计断了这一段，"AI 动过什么"就不是完整的。
// AGENTS.md §8.5 的口径是「没有审计的『成功』不认」—— 被拒的更不该消失。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/audit_test.cpp src/commands.cpp src/config.cpp src/json.cpp \
//     src/text_file.cpp src/str.cpp src/log.cpp -o /tmp/audit_test.exe \
//     -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32
#include <cstdio>
#include <string>

#include <windows.h>

#include "commands.h"
#include "config.h"
#include "text_file.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 临时目录：审计文件不能写到用户的 data\ 里去。
// 用 Win32 的 GetTempPathW，不引新依赖（AGENTS.md §1.2）。
std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return n ? std::wstring(buf, n) : std::wstring(L".");
}

std::wstring ReadAll(const std::wstring& path) {
    std::wstring out;
    if (!tf::ReadText(path, &out)) return out;
    return out;
}

int CountLines(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s)
        if (c == L'\n') ++n;
    return n;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

}  // namespace

int main() {
    // 审计写到独立的临时目录，不碰用户的 data\。
    const std::wstring dir = tf::Join(TempDir(), L"ebb_audit_test");
    CreateDirectoryW(dir.c_str(), nullptr);
    Config::Get().SetProfileDir(dir);
    const std::wstring logPath = tf::Join(dir, L"audit.log");

    std::printf("被拒绝的调用也落审计\n");
    {
        // 清掉可能存在的旧文件，让下面的计数只反映本次。
        DeleteFileW(logPath.c_str());

        const int before = CountLines(ReadAll(logPath));

        // 1) 未知命令
        CommandBus::Get().AuditRejected(L"no.such.cmd", L"unknown_tool");
        // 2) 参数不合法
        CommandBus::Get().AuditRejected(L"view.sort", L"bad_args");
        // 3) 危险未批准
        CommandBus::Get().AuditRejected(L"bmk.clear", L"danger_disabled");

        const std::wstring text = ReadAll(logPath);
        const int after = CountLines(text);

        Check(after - before == 3, "三条拒绝各写一行审计");
        Check(Contains(text, L"rejected=unknown_tool"), "未知命令的原因被记下");
        Check(Contains(text, L"rejected=bad_args"), "参数错误的原因被记下");
        Check(Contains(text, L"rejected=danger_disabled"), "危险未批准的原因被记下");
        Check(Contains(text, L"ok=0"), "拒绝记为 ok=0");

        // 命令名也要在里面，否则事后没法按命令筛。
        Check(Contains(text, L"no.such.cmd"), "命令名被记下");
        Check(Contains(text, L"view.sort"), "命令名被记下（第二条）");
        Check(Contains(text, L"bmk.clear"), "命令名被记下（第三条）");

        // actor 固定为 ai：这些拒绝只来自 AI 通道。
        Check(Contains(text, L" ai "), "actor 记为 ai");

        // 拒绝行必须能被 grep 出来（脚本按 rejected= 筛）。
        Check(Contains(text, L"rejected="), "有 rejected= 标记，便于脚本筛");
    }

    std::printf("拒绝审计与正常审计同构\n");
    {
        // 正常调用走 CommandBus::Audit，格式是 `ok=1` / `ok=0`，没有 rejected=。
        // 两种行要能被区分开 —— 否则脚本没法只筛拒绝。
        const std::wstring text = ReadAll(logPath);
        int rejected = 0, normal = 0;
        // 按行分割：审计行以 "\r\n" 结尾，所以分隔后每行尾部可能带 '\r'，
        // 判前缀时不受影响，但计数必须只算非空行。
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find(L"\n", start);
            if (nl == std::wstring::npos) nl = text.size();
            std::wstring line = text.substr(start, nl - start);
            if (!line.empty()) {
                if (Contains(line, L"rejected=")) ++rejected;
                else ++normal;
            }
            if (nl == text.size()) break;
            start = nl + 1;
        }
        Check(rejected == 3, "三条被识别为拒绝行");
        Check(normal == 0, "本次没有正常调用行（隔离干净）");
    }

    DeleteFileW(logPath.c_str());
    RemoveDirectoryW(dir.c_str());

    std::printf(g_fail ? "\nFAILED %d\n" : "\nPASS\n", g_fail);
    return g_fail ? 1 : 0;
}
