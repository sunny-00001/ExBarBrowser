// audit_log 的纯函数自测（P0-6：审计记录必须是结构化的，且只有一份事实）。
//
// 【为什么这几个点值得单测】
//   · 参数哈希算在**完整**参数上 —— 用截断后的文本算，两组不同的长参数会
//     撞成同一个哈希，"同参重放"分析就废了。这是最容易在重构中被改坏的一条。
//   · JSON 转义必须由唯一的实现来做 —— 手拼一遍转义，迟早漏一个字符，
//     而漏的那个字符会让**整行**解析失败，审计就此静默失灵。
//   · 上下文必须自动恢复 —— 命令会嵌套调用，漏恢复一次，下一条命令就带上
//     上一条的 sessionId。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/audit_log_test.cpp src/audit_log.cpp src/json.cpp src/text_file.cpp \
//     src/str.cpp src/log.cpp src/file_hash.cpp -o /tmp/audit_log_test.exe \
//     -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32 -lbcrypt
#include <cstdio>
#include <string>

#include "audit_log.h"
#include "json.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

std::wstring Repeat(const wchar_t* unit, int times) {
    std::wstring s;
    for (int i = 0; i < times; ++i) s += unit;
    return s;
}

auditlog::Record MakeRecord() {
    auditlog::Record r;
    r.ts = L"2026-11-22T01:02:03.456";
    r.actorKind = L"human";
    r.actorName = L"key";
    r.cmd = L"fs.rename";
    r.argsText = L"{\"target\":\"C:/a.txt\"}";
    r.argsHash = L"0123456789abcdef";
    r.ok = true;
    r.verdict = L"ok";
    r.errorCode.clear();
    r.durationMs = 7;
    r.approval = L"not_required";
    r.approvedBy = L"-";
    r.sessionId = L"s-1";
    r.traceId = L"t-1";
    r.pid = 4242;
    return r;
}

}  // namespace

int main() {
    std::printf("参数哈希：算在完整参数上\n");
    {
        Check(auditlog::HashArgs(L"").empty(), "空参数没有哈希");

        const std::wstring a = L"{\"path\":\"C:/dir/a.txt\"}";
        const std::wstring b = L"{\"path\":\"C:/dir/b.txt\"}";
        const std::wstring h1 = auditlog::HashArgs(a);
        Check(!h1.empty(), "非空参数有哈希");
        Check(h1 == auditlog::HashArgs(a), "同参数哈希稳定");
        Check(h1 != auditlog::HashArgs(b), "不同参数哈希不同");
        Check(h1.size() == 16, "哈希取前 64 位（16 个十六进制字符）");

        // 关键回归：两组长参数**前缀相同**（超过显示截断长度 160），
        // 只有末尾不同。用截断后的文本算哈希的话，这两个会撞成同一个 ——
        // "同参重放"分析于是把两次不同的调用当成一次。
        const std::wstring head = L"{\"path\":\"" + Repeat(L"x", 300);
        const std::wstring long1 = head + L"/a.txt\"}";
        const std::wstring long2 = head + L"/b.txt\"}";
        Check(auditlog::HashArgs(long1) != auditlog::HashArgs(long2),
              "长参数只差末尾也必须算出不同哈希");
    }

    std::printf("JSONL 行：合法且转义正确\n");
    {
        auditlog::Record r = MakeRecord();
        // 参数里塞进引号、反斜杠、换行、控制字符 —— 任何一个漏转义，
        // 整行就解析失败，审计从此静默失灵。
        r.argsText = L"{\"p\":\"a\\\"b\\\\c\\nd\\te\"}";
        const std::wstring line = auditlog::ToJsonLine(r);

        json::Value v;
        std::wstring err;
        const bool parsed = json::Parse(line, v, &err);
        Check(parsed, "含特殊字符的行仍能解析");
        if (parsed) {
            Check(v.GetStr(L"cmd") == L"fs.rename", "字段值正确");
            Check(v.GetStr(L"args") == r.argsText, "转义后往返一致");
            Check(v.GetStr(L"actorKind") == L"human", "actorKind 正确");
            Check(v.GetStr(L"sessionId") == L"s-1", "sessionId 正确");
            Check(v.GetInt(L"durationMs", -1) == 7, "durationMs 正确");
            Check(v.GetInt(L"pid", 0) == 4242, "pid 正确");
            Check(v.GetBool(L"ok", false), "ok 正确");
        }
        Check(line.find(L'\n') == std::wstring::npos, "一行之内没有换行");
    }

    std::printf("文本镜像：字段齐全且能分辨拒绝\n");
    {
        auditlog::Record r = MakeRecord();
        r.ok = false;
        r.verdict = L"rejected";
        r.errorCode = L"danger_disabled";
        const std::wstring line = auditlog::ToTextLine(r);
        Check(Contains(line, L"fs.rename"), "命令名在");
        Check(Contains(line, L"ok=0"), "ok 在");
        // 文本镜像是**旧格式的兼容投影**：被拒调用必须留 `rejected=<机器码>`
        // 这个旧记号（既有脚本按它筛），且 `ok=` 必须在行尾。
        // 第一版把它统一成 "(rejected: …)" 放在 ok= 之后 —— 两个旧锚点同时断。
        Check(Contains(line, L"rejected=danger_disabled"), "旧记号 rejected= 在");
        Check(line.size() >= 4 && line.compare(line.size() - 4, 4, L"ok=0") == 0,
              "ok= 在行尾（老脚本的锚点没断）");
        Check(line.find(L'\r') == std::wstring::npos, "行内没有回车");
    }

    std::printf("上下文：作用域结束自动恢复\n");
    {
        auditlog::Context outer;
        outer.sessionId = L"outer";
        auditlog::Scope s1(outer);
        Check(auditlog::GetContext().sessionId == L"outer", "外层生效");

        {
            auditlog::Context inner;
            inner.sessionId = L"inner";
            inner.approval = L"granted";
            auditlog::Scope s2(inner);
            Check(auditlog::GetContext().sessionId == L"inner", "内层覆盖");
            Check(auditlog::GetContext().approval == L"granted", "内层字段生效");
        }
        // 关键：内层作用域结束后必须回到外层 ——
        // 命令会嵌套调用，漏恢复一次，下一条命令就带上上一条的 sessionId。
        Check(auditlog::GetContext().sessionId == L"outer", "内层出作用域后恢复外层");
        Check(auditlog::GetContext().approval.empty(), "内层的字段没有残留");
    }

    std::printf(g_fail ? "\nFAILED %d\n" : "\nPASS\n", g_fail);
    return g_fail ? 1 : 0;
}
