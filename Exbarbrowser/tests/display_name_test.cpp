// 显示名收敛自测：历史/最近访问不再把 "::{GUID}" 直接当 label。
//
// 编译（ExBarBrowser/ 下）：见 docs/DEVELOPMENT.md §2.4 的 display_name_test 命令。
#include <cstdio>
#include <string>

#include "icon_resolve.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void Eq(const std::wstring& got, const std::wstring& want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) std::printf("        want len=%zu got len=%zu\n", want.size(), got.size());
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 系统命名空间：注册表 LocalizedString 是权威来源，必须能脱离"本次导航"恢复。
    Eq(ico::DisplayName(nav::kThisPc), L"此电脑", "此电脑解析串显示本地化名");
    Eq(ico::DisplayName(L"::{645FF040-5081-101B-9F08-00AA002F954E}"), L"回收站",
       "回收站解析串显示本地化名");
    Eq(ico::DisplayName(L"shell:RecycleBinFolder"), L"回收站", "shell: 别名先归一到解析串");

    // 导航期登记的权威名（第三方 NSE）：优先于注册表，且能保留子项后缀。
    const std::wstring custom = L"::{11111111-2222-3333-4444-555555555555}";
    ico::RememberDisplay(custom, L"测试命名空间");
    Eq(ico::DisplayName(custom), L"测试命名空间", "权威名优先");
    Eq(ico::DisplayName(custom + L"\\子目录"), L"测试命名空间\\子目录",
       "命名空间子项只替换根名");
    Eq(ico::DisplayName(custom, L"Shell 即时名"), L"Shell 即时名", "回调 display 优先");
    Eq(ico::DisplayName(custom, custom), L"测试命名空间", "回调给裸解析串时不覆盖权威名");

    // 文件系统路径保持完整路径（历史下拉与资源管理器一致）。
    Eq(ico::DisplayName(L"C:\\Windows\\System32"), L"C:\\Windows\\System32",
       "文件系统路径原样返回");

    // 兜底不能退化成 "::"。
    Check(ico::DisplayName(L"::{99999999-9999-9999-9999-999999999999}") != L"::",
          "未知命名空间不退化成两个冒号");

    CoUninitialize();
    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
