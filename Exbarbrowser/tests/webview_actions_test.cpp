// WebView2 页面窗口动作的离线回归：白名单、JSON 解析、来源校验。
// 不发消息、不创建窗口，只测纯函数，符合 R21 输入纪律。
#include <cstdio>
#include <string>

#include "aichat_webview_msg.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    using aiui::IsWebViewWindowAction;
    using aiui::ParseWebViewWindowAction;
    using aiui::IsWebViewSourceAllowed;
    using aiui::IsWebViewStatusState;
    using aiui::IsWebViewUndoDomain;
    using aiui::ParseWebViewStatus;

    std::printf("窗口动作白名单\n");
    Check(IsWebViewWindowAction(L"minimize"), "minimize 允许");
    Check(IsWebViewWindowAction(L"close"), "close 允许");
    Check(IsWebViewWindowAction(L"maximize"), "maximize 允许");
    Check(IsWebViewWindowAction(L"pin"), "pin 允许");
    Check(IsWebViewWindowAction(L"unpin"), "unpin 允许");
    Check(!IsWebViewWindowAction(L"drag"), "drag 不允许（子窗口不能拖整窗）");
    Check(!IsWebViewWindowAction(L"navigate"), "未知动作不允许");
    Check(!IsWebViewWindowAction(L""), "空动作不允许");

    std::printf("\n消息解析\n");
    {
        std::wstring action;
        std::wstring error;
        Check(ParseWebViewWindowAction(L"{\"type\":\"window\",\"action\":\"minimize\"}",
                                       &action, &error), "合法 window 消息通过");
        Check(action == L"minimize", "解析出正确动作");
    }
    {
        std::wstring action;
        Check(!ParseWebViewWindowAction(L"{\"type\":\"theme\",\"theme\":\"dark\"}",
                                        &action, nullptr), "非 window 类型被拒绝");
    }
    {
        std::wstring action;
        Check(!ParseWebViewWindowAction(L"{\"type\":\"window\",\"action\":\"drag\"}",
                                        &action, nullptr), "白名单外动作被拒绝");
    }
    {
        std::wstring action;
        Check(!ParseWebViewWindowAction(L"not json", &action, nullptr), "坏 JSON 被拒绝");
    }

    std::printf("\nAI 状态白名单\n");
    Check(IsWebViewStatusState(L"idle"), "idle 允许");
    Check(IsWebViewStatusState(L"busy"), "busy 允许");
    Check(IsWebViewStatusState(L"approval"), "approval 允许");
    Check(IsWebViewStatusState(L"done"), "done 允许");
    Check(IsWebViewStatusState(L"error"), "error 允许");
    Check(!IsWebViewStatusState(L"navigate"), "未知状态不允许");
    Check(!IsWebViewStatusState(L"starting"), "原生启动态不接受页面伪造");
    Check(IsWebViewUndoDomain(L"file"), "file 撤销域允许");
    Check(IsWebViewUndoDomain(L"bookmark"), "bookmark 撤销域允许");
    Check(!IsWebViewUndoDomain(L"settings"), "未知撤销域不允许");
    {
        aiui::AiWebStatus st;
        Check(ParseWebViewStatus(
                  L"{\"type\":\"status\",\"state\":\"done\",\"summary\":\"完成\","
                  L"\"detail\":\"第 3 轮\",\"canUndo\":true,\"undoDomain\":\"file\","
                  L"\"turnId\":3}", &st, nullptr), "合法 status 消息通过");
        Check(st.state == L"done" && st.summary == L"完成" && st.turnId == 3,
              "状态字段解析正确");
        Check(st.canUndo && st.undoDomain == L"file", "撤销域解析正确");
    }
    {
        aiui::AiWebStatus st;
        Check(!ParseWebViewStatus(
                  L"{\"type\":\"status\",\"state\":\"navigate\"}", &st, nullptr),
              "白名单外状态被拒绝");
    }
    {
        aiui::AiWebStatus st;
        Check(!ParseWebViewStatus(
                  L"{\"type\":\"status\",\"state\":\"done\",\"canUndo\":true,"
                  L"\"undoDomain\":\"settings\"}", &st, nullptr),
              "伪造撤销域被拒绝");
    }
    {
        aiui::AiWebStatus st;
        std::wstring longText(500, L'长');
        ParseWebViewStatus(L"{\"type\":\"status\",\"state\":\"error\",\"summary\":\"" + longText +
                               L"\",\"canUndo\":false,\"undoDomain\":\"file\"}", &st, nullptr);
        Check(st.summary.size() <= 121, "过长摘要被裁剪");
        Check(!st.canUndo && st.undoDomain.empty(), "不可撤销时清空撤销域");
    }

    std::printf("\n来源校验\n");
    const std::wstring origin = L"http://127.0.0.1:4319";
    Check(IsWebViewSourceAllowed(origin, origin + L"/?exbarHttpToken=t"), "同 origin 查询串通过");
    Check(IsWebViewSourceAllowed(origin, origin + L"/index.html"), "同 origin 路径通过");
    Check(!IsWebViewSourceAllowed(origin, L"http://127.0.0.1:4319.evil.com/"),
          "端口后接域名不算同 origin");
    Check(!IsWebViewSourceAllowed(origin, L"https://127.0.0.1:4319/"), "协议不同被拒绝");
    Check(!IsWebViewSourceAllowed(L"", origin + L"/"), "未设置来源白名单时拒绝");

    std::printf("\n结果: %d 失败\n", g_fail);
    return g_fail;
}
