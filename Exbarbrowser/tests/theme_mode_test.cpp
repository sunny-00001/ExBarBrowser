// 深浅色档位的纯函数自测：档位 -> "要不要深色"。
//
// 【为什么值得单测】这条判据有三个调用点（Theme::Init、WM_SETTINGCHANGE、
// WM_THEMECHANGED），而三条路径各写一遍 `mode==dark || (mode==system && SystemIsDark())`
// 就会分叉 —— 分叉的表现是"强制浅色时系统切一次深色，本窗口又变回深色"，
// 而这种表现跟"配置没保存"长得一模一样，极难反推到判据上。
//
// 只测**确定的分支**（dark / light / 认不出的值）：system 那一路要读注册表，
// 结果随机器而变，钉死它等于让测试在别的机器上随机红。
#include <cstdio>
#include <string>

#include "theme.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

int main() {
    Check(thememode::kSystem == std::wstring(L"system"), "system 的稳定标识是 \"system\"");
    Check(thememode::kDark == std::wstring(L"dark"), "dark 的稳定标识是 \"dark\"");
    Check(thememode::kLight == std::wstring(L"light"), "light 的稳定标识是 \"light\"");

    Check(ModePrefersDark(thememode::kDark), "强制深色 -> 深色（与系统无关）");
    Check(!ModePrefersDark(thememode::kLight), "强制浅色 -> 浅色（与系统无关）");

    // 认不出的值按 system 处理：配置被手改坏时跟随系统，
    // 比固定某一档更不容易让人以为程序坏了。
    Check(ModePrefersDark(L"Dark") == ModePrefersDark(thememode::kSystem),
          "大小写写错按 system 处理（不静默固定某一档）");
    Check(ModePrefersDark(L"") == ModePrefersDark(thememode::kSystem),
          "空串按 system 处理");
    Check(ModePrefersDark(L"bogus") == ModePrefersDark(thememode::kSystem),
          "认不出的取值按 system 处理");

    if (g_fail) {
        std::printf("theme_mode_test: %d 失败\n", g_fail);
        return 1;
    }
    std::printf("全部通过\n");
    return 0;
}