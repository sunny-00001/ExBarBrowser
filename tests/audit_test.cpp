// 审计自测（E4：被拒绝的调用也必须留痕；P0-6：审计是结构化的）。
//
// 【守的是什么】AI 通道有一整段在**进 CommandBus 之前**就返回了：
// 未知命令 / 未暴露 / 未实现 / schema 非法 / 参数不合法 / 危险未批准。
// 这些拒绝过去**一条审计都不写** —— 实测：连续 4 次被拒 → audit.log 0 增长。
//
// 【为什么这是真问题】被拒的调用恰恰最值得追的：
//   · 反复撞同一个错 → 调用方（或模型）用错了方法；
//   · 反复撞 danger_disabled → 有人在试探破坏性操作的边界；
//   · 审计断了这一段，"AI 动过什么"就不是完整的。
// AGENTS.md §8.5 的口径是「没有审计的『成功』不认」—— 被拒的更不该消失。
//
// 【这一版验的是结构化契约】审计现在有两份投影：
//   · logs\audit.jsonl —— 规范格式（R18 要求的那份），一行一条 JSON
//   · logs\audit.log   —— 旧文本格式的镜像，给既有脚本 grep
// 两者来自**同一条记录**，所以必须同时断言"JSONL 里有结构化字段"
// 与"文本行里仍能分辨拒绝"—— 只验一份，另一份坏了没人知道。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/audit_test.cpp src/commands.cpp src/audit_log.cpp src/config.cpp \
//     src/json.cpp src/text_file.cpp src/str.cpp src/log.cpp src/file_hash.cpp \
//     src/cmd_naming.cpp -o /tmp/audit_test.exe \
//     -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32 -lbcrypt
#include <cstdio>
#include <string>

#include <windows.h>

#include "audit_log.h"
#include "commands.h"
#include "config.h"
#include "json.h"
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

// 取第 index 行（0 起）。审计行以 \n 结尾。
std::wstring LineAt(const std::wstring& text, int index) {
    size_t start = 0;
    for (int i = 0;; ++i) {
        const size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) return std::wstring();
        if (i == index) return text.substr(start, nl - start);
        start = nl + 1;
    }
}

}  // namespace

int main() {
    // 审计写到独立的临时目录，不碰用户的 data\。
    const std::wstring dir = tf::Join(TempDir(), L"ebb_audit_test");
    CreateDirectoryW(dir.c_str(), nullptr);
    Config::Get().SetProfileDir(dir);
    const std::wstring logs = tf::Join(dir, L"logs");
    tf::EnsureDir(logs);
    const std::wstring logPath = tf::Join(logs, L"audit.log");
    const std::wstring jsonlPath = tf::Join(logs, L"audit.jsonl");

    std::printf("被拒绝的调用也落审计（规范格式 audit.jsonl）\n");
    {
        // 清掉可能存在的旧文件，让下面的计数只反映本次。
        DeleteFileW(logPath.c_str());
        DeleteFileW(jsonlPath.c_str());

        // 1) 未知命令
        CommandBus::Get().AuditRejected(L"no.such.cmd", L"未知命令", L"unknown_tool");
        // 2) 参数不合法
        CommandBus::Get().AuditRejected(L"view.sort", L"参数非法", L"bad_args");
        // 3) 危险未批准
        CommandBus::Get().AuditRejected(L"bmk.clear", L"危险未允许", L"danger_disabled");

        const std::wstring jsonl = ReadAll(jsonlPath);
        Check(CountLines(jsonl) == 3, "三条拒绝各写一行 audit.jsonl");

        // 每行都必须是**合法 JSON** —— 结构化审计的全部价值就在这儿。
        // 解析失败说明转义漏了字符（引号、反斜杠、控制字符都能破）。
        bool allParsed = true;
        for (int i = 0; i < 3; ++i) {
            json::Value v;
            std::wstring err;
            if (!json::Parse(LineAt(jsonl, i), v, &err) || !v.IsObject()) allParsed = false;
        }
        Check(allParsed, "三行都是合法 JSON");

        // 字段齐全：这是 C6（actor / 命令 / 参数哈希 / 结果 / 耗时 / 是否审批）
        // 与 P1-3 要求的字段集。少一个就有一类问题永远只能靠猜。
        json::Value first;
        std::wstring err;
        json::Parse(LineAt(jsonl, 0), first, &err);
        Check(first.GetStr(L"actorKind") == L"ai", "actorKind=ai");
        Check(first.GetStr(L"actorName") == L"ai", "actorName=ai");
        Check(first.GetStr(L"cmd") == L"no.such.cmd", "命令名被记下");
        Check(first.GetStr(L"verdict") == L"rejected", "verdict=rejected");
        Check(first.GetStr(L"errorCode") == L"unknown_tool", "errorCode 是机器可判读的分类");
        Check(!first.GetBool(L"ok", true), "拒绝记为 ok=false");
        Check(first.GetInt(L"pid", 0) > 0, "pid 被记下");
        Check(!first.GetStr(L"ts").empty(), "时间戳被记下");
        Check(first.Find(L"durationMs") != nullptr, "耗时字段在");
        Check(first.Find(L"approval") != nullptr, "审批字段在");
        Check(first.Find(L"sessionId") != nullptr, "sessionId 字段在");
        Check(first.Find(L"traceId") != nullptr, "traceId 字段在");
        Check(first.Find(L"argsHash") != nullptr, "参数哈希字段在");

        // 三条的错误分类要各不相同 —— 分类的意义就是能分开处理。
        Check(Contains(jsonl, L"\"errorCode\":\"bad_args\""), "bad_args 分类被记下");
        Check(Contains(jsonl, L"\"errorCode\":\"danger_disabled\""), "danger_disabled 分类被记下");
    }

    std::printf("文本镜像 audit.log 仍可 grep，且能分辨拒绝\n");
    {
        const std::wstring text = ReadAll(logPath);
        Check(CountLines(text) == 3, "三条拒绝各写一行文本镜像");

        int rejected = 0;
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find(L"\n", start);
            if (nl == std::wstring::npos) nl = text.size();
            std::wstring line = text.substr(start, nl - start);
            if (!line.empty() && Contains(line, L"rejected=")) ++rejected;
            if (nl == text.size()) break;
            start = nl + 1;
        }
        Check(rejected == 3, "三条被识别为拒绝行");
        // 文本镜像必须保留**旧版记号** `rejected=<机器码>` ——
        // 既有脚本按它筛被拒调用（旧版 audit_test 就是这么断言的）。
        // 第一版实现把它改成了 "(rejected: …)"，那个记号就此消失，
        // 旧脚本一条拒绝也看不见 —— "新格式更整齐"换"旧脚本失效"是净亏。
        // 人可读的原因与机器码的完整结构在 audit.jsonl（上面已断言 errorCode）。
        Check(Contains(text, L"rejected=unknown_tool"), "未知命令的原因被记下");
        Check(Contains(text, L"rejected=bad_args"), "参数错误的原因被记下");
        Check(Contains(text, L"rejected=danger_disabled"), "危险未批准的原因被记下");
        Check(Contains(text, L" ai "), "actor 记为 ai");
        Check(Contains(text, L"no.such.cmd"), "命令名被记下");

        // `ok=` 必须在**行尾**：既有脚本用 `ok=0$` 锚定失败行。
        // 第一版把 "(verdict: 原因)" 放在 ok= 之后，锚点就断了。
        bool allOkAtEnd = true;
        size_t lstart = 0;
        while (lstart <= text.size()) {
            size_t lnl = text.find(L"\n", lstart);
            if (lnl == std::wstring::npos) lnl = text.size();
            const std::wstring line = text.substr(lstart, lnl - lstart);
            if (!line.empty() && !Contains(line, L"ok=0") && !Contains(line, L"ok=1")) {
                allOkAtEnd = false;
            }
            if (lnl == text.size()) break;
            lstart = lnl + 1;
        }
        Check(allOkAtEnd, "每一行都以 ok= 标记收尾（老脚本的锚点没断）");
    }

    std::printf("上下文里的 sessionId / traceId / 审批会进记录\n");
    {
        DeleteFileW(jsonlPath.c_str());
        auditlog::Context c;
        c.sessionId = L"sess-42";
        c.traceId = L"req-7";
        c.approval = L"granted";
        c.approvedBy = L"ai";
        auditlog::Scope scope(c);

        auditlog::Record rec;
        rec.ts = auditlog::NowIso();
        rec.actorKind = L"ai";
        rec.actorName = L"ai";
        rec.cmd = L"fs.delete";
        rec.ok = true;
        rec.verdict = L"ok";
        rec.durationMs = 12;
        rec.approval = auditlog::GetContext().approval;
        rec.approvedBy = auditlog::GetContext().approvedBy;
        rec.sessionId = auditlog::GetContext().sessionId;
        rec.traceId = auditlog::GetContext().traceId;
        auditlog::Write(rec);

        const std::wstring jsonl = ReadAll(jsonlPath);
        Check(Contains(jsonl, L"\"sessionId\":\"sess-42\""), "sessionId 进记录");
        Check(Contains(jsonl, L"\"traceId\":\"req-7\""), "traceId 进记录");
        Check(Contains(jsonl, L"\"approval\":\"granted\""), "审批状态进记录");
        Check(Contains(jsonl, L"\"durationMs\":12"), "耗时进记录");
    }

    DeleteFileW(logPath.c_str());
    DeleteFileW(jsonlPath.c_str());
    RemoveDirectoryW(dir.c_str());

    std::printf(g_fail ? "\nFAILED %d\n" : "\nPASS\n", g_fail);
    return g_fail ? 1 : 0;
}
