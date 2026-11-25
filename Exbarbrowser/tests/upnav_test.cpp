// 导航路径语义自测（nav_path.h）。
//
// 钉死的行为：盘符根**能**向上（去此电脑），这是"盘根双击空白处无反应"的
// 核心回归断言；命名空间与 UNC 根**不能**（父级是桌面/无更上级，属意外跳转）。
// 同时验证与旧 NavModel 实现合并后没有丢行为（超长路径不再失效）。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc -static \
//     tests/upnav_test.cpp -o /tmp/upnav_test.exe
#include <cstdio>
#include <string>

#include "nav_path.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void ExpectUp(const wchar_t* p, bool want) {
    const bool got = nav::CanNavigateUp(p);
    std::printf("  %s  CanNavigateUp(\"%ls\") = %d\n", got == want ? "PASS" : "FAIL", p, got);
    if (got != want) ++g_fail;
}

void ExpectEq(const std::wstring& got, const wchar_t* want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        std::printf("        期望 \"%ls\"，得到 \"%ls\"\n", want, got.c_str());
        ++g_fail;
    }
}

}  // namespace

int main() {
    std::printf("盘符根：可向上（去此电脑）\n");
    {
        ExpectUp(L"C:\\", true);
        ExpectUp(L"D:\\", true);
        ExpectUp(L"c:\\", true);   // 小写盘符同样是盘根
        ExpectUp(L"C:", true);     // 不带尾斜杠的写法
        ExpectEq(nav::UpTarget(L"C:\\"), nav::kThisPc, "C:\\ 的上一级是此电脑");
        ExpectEq(nav::UpTarget(L"E:"), nav::kThisPc, "E: 的上一级也是此电脑");
    }

    std::printf("\n普通目录：父目录非空即可向上\n");
    {
        ExpectUp(L"C:\\Users", true);
        ExpectUp(L"D:\\Windhawk\\ExBarBrowser", true);
        ExpectUp(L"\\\\server\\share\\dir", true);   // UNC 下的子目录
        ExpectEq(nav::UpTarget(L"C:\\Windows"), L"C:\\", "C:\\Windows -> C:\\");
        ExpectEq(nav::ParentOf(L"\\\\server\\share\\dir"), L"\\\\server\\share",
                 "UNC 子目录的父级是 UNC 根");
    }

    std::printf("\nUNC 根与命名空间：没有更上级\n");
    {
        ExpectUp(L"\\\\server\\share", false);
        ExpectUp(L"\\\\server\\share\\", false);
        ExpectUp(nav::kThisPc, false);
        ExpectUp(L"shell:RecycleBinFolder", false);
        ExpectUp(L"", false);
        ExpectEq(nav::UpTarget(L"\\\\server\\share"), L"", "UNC 根无去处");
        ExpectEq(nav::UpTarget(nav::kThisPc), L"", "此电脑无去处");
    }

    std::printf("\nIsDriveRoot 只认盘根本身\n");
    {
        Check(nav::IsDriveRoot(L"C:\\"), "C:\\ 是盘根");
        Check(nav::IsDriveRoot(L"C:"), "C: 是盘根");
        Check(!nav::IsDriveRoot(L"C:\\Windows"), "C:\\Windows 不是盘根");
        Check(!nav::IsDriveRoot(L"1:\\"), "1:\\ 不是合法盘符");
        Check(!nav::IsDriveRoot(L"\\\\server\\share"), "UNC 根不是盘符根");
    }

    std::printf("\nLeaf：标签标题用\n");
    {
        ExpectEq(nav::Leaf(L"C:\\Windows"), L"Windows", "末级名");
        ExpectEq(nav::Leaf(L"C:\\Windows\\"), L"Windows", "尾斜杠不影响");
        ExpectEq(nav::Leaf(L"C:\\"), L"C:\\", "盘根保留完整形式（不是空）");
        ExpectEq(nav::Leaf(L"C:"), L"C:\\", "C: 补成 C:\\");
        ExpectEq(nav::Leaf(L"\\\\srv\\share\\dir"), L"dir", "UNC 子目录的末级名");
        ExpectEq(nav::Leaf(nav::kThisPc), nav::kThisPc, "命名空间原样返回");
        ExpectEq(nav::Leaf(L""), L"", "空路径返回空");
    }

    std::printf("\nNormalize：正斜杠与尾斜杠\n");
    {
        ExpectEq(nav::Normalize(L"C:/a/b/"), L"C:\\a\\b", "正转反、去尾斜杠");
        ExpectEq(nav::Normalize(L"C:\\"), L"C:\\", "盘根尾斜杠必须保留");
        ExpectEq(nav::Normalize(nav::kThisPc), nav::kThisPc, "命名空间不动");
    }

    std::printf("\n超长路径（>260）：旧 NavModel 实现会在此静默失效\n");
    {
        // 拼一条 300+ 字符的深路径
        std::wstring deep = L"C:\\";
        while (deep.size() < 320) deep += L"verylongsegment\\";
        deep.pop_back();  // 去掉尾斜杠
        Check(deep.size() > 260, "构造出的路径确实超过 MAX_PATH");
        Check(nav::CanNavigateUp(deep), "超长路径仍然可向上");
        const std::wstring up = nav::UpTarget(deep);
        Check(!up.empty() && up.size() < deep.size(), "父级非空且更短");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
