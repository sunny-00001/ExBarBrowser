// 通道自举单测：--ai / EXBARBROWSER_AI 的"强制开启"必须是纯进程内标志。
//
// 判据（不是"环境变量读到了"，而是不会污染用户设置）：
//   · 默认不强制时，AiChannelEnabled 跟 settings.json 的 aiEnabled 一致；
//   · 强制开启后 AiChannelEnabled=true，但 s().aiEnabled 仍是原值；
//   · Save() 即使用户设置是 false，也不会把强制标志写进 settings.json
//     —— 否则测试实例关闭就把用户的 aiEnabled 永久翻成 true（实测踩过）；
//   · 强制标志不随 SetProfileDir 重置（它在 main 每次启动只设一次）。
#include <cstdio>
#include <string>

#include "common.h"
#include "config.h"
#include "json.h"
#include "text_file.h"

static int g_pass = 0, g_fail = 0;
static void Expect(bool cond, const char* what) {
    if (cond) ++g_pass;
    else { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

int main() {
    std::printf("通道自举（--ai 强制开启，不写盘）\n");

    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = tf::Join(std::wstring(tmp), L"exbar-ai-force-test");
    tf::EnsureDir(dir);
    const std::wstring sp = tf::Join(dir, L"data\\settings.json");
    DeleteFileW(tf::ToExtended(sp).c_str());

    Config& cfg = Config::Get();
    cfg.SetProfileDir(dir);
    cfg.SetAiForceEnabled(false);
    cfg.Load();

    // 1. 默认不强制：设置里 aiEnabled 默认 false → 通道关
    cfg.mutableSettings().aiEnabled = false;
    Expect(cfg.AiChannelEnabled() == false, "默认：通道关");

    // 2. 设置里 aiEnabled=true（无强制）→ 通道开
    cfg.mutableSettings().aiEnabled = true;
    Expect(cfg.AiChannelEnabled() == true, "设置开启：通道开");
    cfg.mutableSettings().aiEnabled = false;

    // 3. 强制开启：通道开，但 s().aiEnabled 不变（关键：不碰持久设置）
    cfg.SetAiForceEnabled(true);
    Expect(cfg.AiChannelEnabled() == true, "强制开启：通道开");
    Expect(cfg.s().aiEnabled == false, "强制开启：s().aiEnabled 仍是 false");

    // 4. Save() 后 settings.json 里 aiEnabled 仍是 false（强制标志不落盘）
    cfg.Save();
    std::wstring text;
    tf::ReadText(sp, &text);
    Expect(text.find(L"\"aiEnabled\": true") == std::wstring::npos,
           "Save 不把强制标志写进 settings.json");

    // 5. 强制标志不随 SetProfileDir 重置
    const std::wstring dir2 = tf::Join(std::wstring(tmp), L"exbar-ai-force-test-b");
    tf::EnsureDir(dir2);
    cfg.SetProfileDir(dir2);
    Expect(cfg.AiChannelEnabled() == true, "SetProfileDir 后强制标志仍在");

    cfg.SetAiForceEnabled(false);
    std::printf("通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
