// 测试隔离单测：第二个实例（MULTI=1 / 显式 PROFILE）必须完全落在自己的 profile 根。
//
// 判据不是"环境变量读到了"，而是：
//   · profile 根下六类目录（data/state/logs/cache/runtime，加根本身）各自派生正确；
//   · 配置/状态/书签/发现文件都跟着 profile 走，不写进默认用户的目录；
//   · 切目录后内存配置必须重置，否则会把 A 的配置写回 B。
#include <cstdio>
#include <string>

#include "common.h"
#include "config.h"
#include "text_file.h"

static int g_pass = 0, g_fail = 0;
static void Expect(bool cond, const char* what) {
    if (cond) ++g_pass;
    else { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

int main() {
    std::printf("测试隔离（独立 profile）\n");

    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    // 目录名带 PID：多会话/并行跑测试时，固定目录会被另一轮同时改写。
    // 【不带的代价是实测出来的】本测试开头只清理 profile **a** 的文件，不清理 b；
    // 而结尾会往 b\data\settings.json 写 viewMode=1。于是**第二轮起**，
    // 换到 b 后 Load() 读到的就是上一轮自己留下的 viewMode=1，
    // "换目录后配置重置为默认"断言必然失败（实测：首轮绿、次轮红）。
    // 与 config_roundtrip_test 同一口径：每轮用独立目录，残留无从继承。
    const std::wstring base =
        tf::Join(std::wstring(tmp), L"exbar-profile-test_" + std::to_wstring(GetCurrentProcessId()));
    const std::wstring a = tf::Join(base, L"a");
    const std::wstring b = tf::Join(base, L"b");
    tf::EnsureDir(a);
    tf::EnsureDir(b);

    Config& cfg = Config::Get();
    cfg.SetProfileDir(a);
    // 测试必须可重复跑：上轮换目录留下的偏好/状态会让"默认值"断言假失败。
    for (const auto& p : {cfg.SettingsPath(), cfg.SessionPath(), cfg.HistoryPath(),
                          cfg.TabsPath(), cfg.WindowPath()})
        DeleteFileW(tf::ToExtended(p).c_str());
    cfg.Load();

    Expect(cfg.RootDir() == a, "RootDir 跟随 profile");
    Expect(StartsWith(cfg.Dir(), a), "data 在 profile 内");
    Expect(StartsWith(cfg.StateDir(), a), "state 在 profile 内");
    Expect(StartsWith(cfg.LogsDir(), a), "logs 在 profile 内");
    Expect(StartsWith(cfg.CacheDir(), a), "cache 在 profile 内");
    Expect(StartsWith(cfg.RuntimeDir(), a), "runtime 在 profile 内");
    Expect(StartsWith(cfg.SettingsPath(), a), "settings.json 在 profile 内");
    Expect(StartsWith(cfg.BookmarksPath(), a), "bookmarks.html 在 profile 内");
    Expect(StartsWith(cfg.KeymapPath(), a), "keymap.json 在 profile 内");
    Expect(StartsWith(cfg.MousePath(), a), "mouse.json 在 profile 内");
    Expect(StartsWith(cfg.SessionPath(), a), "session.json 在 profile 内");
    Expect(StartsWith(cfg.WindowPath(), a), "window.json 在 profile 内");

    // 写一份带标记的配置，再切到 b：必须读到默认值，不能把 a 的标记带过去。
    cfg.mutableSettings().viewMode = 6;
    cfg.Save();
    Expect(tf::Exists(tf::Join(a, L"data\\settings.json")), "写盘落在 a\\data，不在用户 data");
    Expect(tf::Exists(tf::Join(a, L"state\\session.json")), "状态落在 a\\state");
    cfg.SetProfileDir(b);
    cfg.Load();
    Expect(cfg.s().viewMode == 3, "换目录后配置重置为默认（没串 profile）");
    cfg.mutableSettings().viewMode = 1;
    cfg.Save();
    Expect(tf::Exists(tf::Join(b, L"data\\settings.json")), "第二次写盘落在 b\\data");

    json::Value ja, jb;
    std::wstring text, err;
    Expect(tf::ReadText(tf::Join(a, L"data\\settings.json"), &text) &&
               json::Parse(text, ja, &err),
           "a 的配置能读回");
    Expect(tf::ReadText(tf::Join(b, L"data\\settings.json"), &text) &&
               json::Parse(text, jb, &err),
           "b 的配置能读回");
    Expect(ja.GetInt(L"viewMode", -1) == 6 && jb.GetInt(L"viewMode", -1) == 1,
           "两个 profile 互不覆盖");

    // 恢复默认 profile 根，避免同进程里的后续测试受影响。
    cfg.SetProfileDir(tf::ExeDir());
    cfg.Load();

    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
