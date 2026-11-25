// 测试隔离单测：第二个实例（MULTI=1 / 显式 PROFILE）必须完全落在自己的目录。
//
// 判据不是"环境变量读到了"，而是：
//   · 所有派生路径（设置/书签/键位/鼠标/审计/AI 发现文件）都跟着 profile 走；
//   · 切目录后内存配置必须重置，否则会把 A 的配置写回 B；
//   · 用户默认 data 目录里的文件一个字节都不动。
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
    const std::wstring base = tf::Join(std::wstring(tmp), L"exbar-profile-test");
    const std::wstring a = tf::Join(base, L"a");
    const std::wstring b = tf::Join(base, L"b");
    tf::EnsureDir(a);
    tf::EnsureDir(b);
    // 测试必须可重复跑：上次留下的 settings.json 会让"默认值"断言假失败。
    DeleteFileW(tf::ToExtended(tf::Join(a, L"settings.json")).c_str());
    DeleteFileW(tf::ToExtended(tf::Join(b, L"settings.json")).c_str());

    Config& cfg = Config::Get();
    cfg.SetProfileDir(a);
    cfg.Load();

    Expect(StartsWith(cfg.Dir(), a), "Dir 跟随 profile");
    Expect(StartsWith(cfg.SettingsPath(), a), "settings.json 在 profile 内");
    Expect(StartsWith(cfg.BookmarksPath(), a), "bookmarks.html 在 profile 内");
    Expect(StartsWith(cfg.KeymapPath(), a), "keymap.json 在 profile 内");
    Expect(StartsWith(cfg.MousePath(), a), "mouse.json 在 profile 内");

    // 写一份带标记的配置，再切到 b：必须读到默认值，不能把 a 的标记带过去。
    cfg.mutableSettings().viewMode = 6;
    cfg.Save();
    Expect(tf::Exists(tf::Join(a, L"settings.json")), "写盘落在 a，不在用户 data");
    cfg.SetProfileDir(b);
    cfg.Load();
    Expect(cfg.s().viewMode == 3, "换目录后配置重置为默认（没串 profile）");
    cfg.mutableSettings().viewMode = 1;
    cfg.Save();
    Expect(tf::Exists(tf::Join(b, L"settings.json")), "第二次写盘落在 b");

    json::Value ja, jb;
    std::wstring text, err;
    Expect(tf::ReadText(tf::Join(a, L"settings.json"), &text) && json::Parse(text, ja, &err),
           "a 的配置能读回");
    Expect(tf::ReadText(tf::Join(b, L"settings.json"), &text) && json::Parse(text, jb, &err),
           "b 的配置能读回");
    Expect(ja.GetInt(L"viewMode", -1) == 6 && jb.GetInt(L"viewMode", -1) == 1,
           "两个 profile 互不覆盖");

    // 恢复默认目录，避免同进程里的后续测试受影响。
    cfg.SetProfileDir(tf::Join(tf::ExeDir(), L"data"));
    cfg.Load();

    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
