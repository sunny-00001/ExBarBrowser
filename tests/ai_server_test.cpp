// AI 通道协议自测：HandleRequestLine 是纯函数（不碰窗口/管道），可直接调。
//
// 覆盖的是"外部程序能拿到的响应形状" —— 这里错了，客户端会解析失败或
// 误判成功，而 A 侧看不出任何异常。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/ai_server_test.cpp src/ai_server.cpp src/ai_api.cpp src/commands.cpp \
//     src/json.cpp src/config.cpp src/text_file.cpp \
//     -o /tmp/ai_server_test.exe -lole32 -luuid -lshell32 -ladvapi32
//
// 注意：本测试不启动监听（不调 StartServer），只测请求→响应的纯逻辑。
// 令牌校验依赖进程内生成的值，未启动时为空 → 一律 bad_token，这正是要验证的
// fail-closed 行为。
#include <cstdio>
#include <string>

#include "ai_server.h"
#include "json.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// 解析响应并取字段
bool RespIs(const std::wstring& resp, const wchar_t* key, std::wstring* val) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(resp, v, &err)) return false;
    *val = v.GetStr(key);
    return true;
}

bool RespOk(const std::wstring& resp, bool* ok) {
    json::Value v;
    std::wstring err;
    if (!json::Parse(resp, v, &err)) return false;
    *ok = v.GetBool(L"ok", false);
    return true;
}

}  // namespace

int main() {
    std::printf("非法输入\n");
    {
        std::wstring r = ai::HandleRequestLine(L"not json at all");
        std::wstring e;
        Check(RespIs(r, L"error", &e) && e == L"invalid_json", "非 JSON -> invalid_json");

        r = ai::HandleRequestLine(L"[1,2,3]");
        Check(RespIs(r, L"error", &e) && e == L"invalid_json", "非对象 -> invalid_json");

        r = ai::HandleRequestLine(L"\"just a string\"");
        Check(RespIs(r, L"error", &e) && e == L"invalid_json", "字符串 -> invalid_json");
    }

    std::printf("\n令牌校验（未启用通道时必须拒绝一切）\n");
    {
        // 未调 StartServer：g_token 为空 → 任何请求都该被拒
        std::wstring r = ai::HandleRequestLine(L"{\"token\":\"x\",\"method\":\"state\"}");
        std::wstring e;
        Check(RespIs(r, L"error", &e) && e == L"bad_token", "错误令牌 -> bad_token");

        r = ai::HandleRequestLine(L"{\"method\":\"state\"}");
        Check(RespIs(r, L"error", &e) && e == L"bad_token", "缺令牌 -> bad_token");

        r = ai::HandleRequestLine(L"{\"token\":\"\",\"method\":\"tools/list\"}");
        Check(RespIs(r, L"error", &e) && e == L"bad_token", "空令牌 -> bad_token");

        // 关键：令牌错时不能泄露"有哪些工具"
        if (RespIs(r, L"error", &e))
            Check(e == L"bad_token" && r.find(L"tools") == std::wstring::npos,
                  "bad_token 响应里不含工具清单（不泄露能力面）");
    }

    std::printf("\n令牌文件路径\n");
    {
        std::wstring p = ai::TokenPath();
        Check(p.find(L"ai-token.txt") != std::wstring::npos, "TokenPath 指向 ai-token.txt");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
