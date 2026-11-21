// 导航栏输入语义自测（nav_path.h 的纯函数部分）。
//
// 钉死的是三类"看起来能用、实际是死的"输入：
//   ① 面包屑分段：UNC 与命名空间子项以前塌成一段（"能上不能点"）
//   ② 用户输入清理：`"C:\Program Files"` 这类带引号文本以前直接 E_INVALIDARG
//   ③ Tab 补全切分：盘根 `D:\` 与 UNC 半截 `\\server\sh` 以前切不出目录
//
// 编译（在 ExBarBrowser/ 下）：
//   "/c/Program Files/Windhawk/Compiler/bin/clang++.exe" -std=c++20 -O1 \
//     -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 \
//     -Isrc -static tests/nav_input_test.cpp -o /tmp/nav_input_test.exe
//   /tmp/nav_input_test.exe
#include <cstdio>
#include <string>
#include <vector>

#include "nav_path.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void Eq(const std::wstring& got, const std::wstring& want, const char* what) {
    const bool ok = got == want;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) std::printf("        期望 \"%ls\"，得到 \"%ls\"\n", want.c_str(), got.c_str());
    if (!ok) ++g_fail;
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L"|";
        s += v[i];
    }
    return s;
}

}  // namespace

int main() {
    std::printf("面包屑分段：盘符路径\n");
    {
        const auto p = nav::Prefixes(L"C:\\Windows\\System32");
        Eq(Join(p), L"C:\\|C:\\Windows|C:\\Windows\\System32", "三级盘符路径");
        Eq(nav::CrumbLabel(p[0], 0), L"C:", "首段标签是盘符（不带反斜杠）");
        Eq(nav::CrumbLabel(p[1], 1), L"Windows", "次段标签是末级名");
        Check(nav::Prefixes(L"C:\\").size() == 1, "盘根只有一段");
        Eq(Join(nav::Prefixes(L"C:\\")), L"C:\\", "盘根整串");
    }

    std::printf("\n面包屑分段：UNC（以前整条塌成一段）\n");
    {
        const auto p = nav::Prefixes(L"\\\\srv\\share\\a\\b");
        Eq(Join(p), L"\\\\srv\\share|\\\\srv\\share\\a|\\\\srv\\share\\a\\b",
           "UNC 以共享根为首段");
        Check(p.size() == 3, "UNC 分了 3 段（不是一个）");
        // 共享根本身是可达目标（ParentOf 也这么认为）
        Eq(nav::UpTarget(L"\\\\srv\\share\\a"), L"\\\\srv\\share", "共享根确实可作上一级");
        Eq(Join(nav::Prefixes(L"\\\\srv\\share")), L"\\\\srv\\share", "共享根自身只有一段");
    }

    std::printf("\n面包屑分段：命名空间与相对路径不切\n");
    {
        Eq(Join(nav::Prefixes(nav::kThisPc)), nav::kThisPc, "命名空间原样单段");
        Eq(Join(nav::Prefixes(L"shell:RecycleBinFolder")), L"shell:RecycleBinFolder",
           "shell: 命名空间单段");
        Eq(Join(nav::Prefixes(L"relative\\dir")), L"relative\\dir",
           "相对路径不猜基准，单段");
        Check(nav::Prefixes(L"").empty(), "空路径没有段");

        // 【关键回归】命名空间标签不能被误判成盘符。
        // "::{GUID}" 的 s[1] 也是 ':'，旧 CrumbLabel 用 s.substr(0,2) 得到 "::"，
        // 导航到「此电脑」「回收站」时地址栏面包屑显示成 ">::>"（箭头 + "::" + 箭头）。
        Eq(nav::CrumbLabel(nav::kThisPc, 0), nav::kThisPc,
           "此电脑标签是完整解析串，不是被截短的 '::'");
        Eq(nav::CrumbLabel(L"::{645FF040-5081-101B-9F08-00AA002F954E}", 0),
           L"::{645FF040-5081-101B-9F08-00AA002F954E}", "回收站标签同理");
        Eq(nav::CrumbLabel(L"shell:RecycleBinFolder", 0), L"shell:RecycleBinFolder",
           "shell: 命名空间标签原样");
        Check(nav::CrumbLabel(nav::kThisPc, 0) != L"::", "绝不能只剩两个冒号");
        // 盘根判定收紧后，真正的盘符仍然要被切成 "C:"
        Eq(nav::CrumbLabel(L"C:\\", 0), L"C:", "盘根标签不受影响");
        Eq(nav::CrumbLabel(L"C:\\Windows", 1), L"Windows", "非首段标签仍是末级名");
    }

    std::printf("\n用户输入清理：引号 / 空白 / 正斜杠\n");
    {
        Eq(nav::FromUserText(L"  \"C:\\Program Files\"  "), L"C:\\Program Files",
           "剥引号与首尾空白（资源管理器「复制为路径」的格式）");
        Eq(nav::FromUserText(L"'C:\\Temp'"), L"C:\\Temp", "单引号也剥");
        Eq(nav::FromUserText(L"C:/Windows/"), L"C:\\Windows", "正斜杠转反斜杠并去尾斜杠");
        Eq(nav::FromUserText(L"\tC:\\A\r\n"), L"C:\\A", "制表与换行属于首尾空白");
        Eq(nav::FromUserText(L"C:\\\u3000"), L"C:\\", "全角空格也算空白");
        Eq(nav::FromUserText(L"\"\"C:\\x\"\""), L"\"C:\\x\"", "只剥一层配对引号");
        Eq(nav::FromUserText(L"\"C:\\x"), L"\"C:\\x", "不配对的引号不动");
        Eq(nav::FromUserText(L"C:\\"), L"C:\\", "盘根尾斜杠保留");
        Eq(nav::FromUserText(nav::kThisPc), nav::kThisPc, "命名空间不动");
        Eq(nav::FromUserText(L""), L"", "空串仍是空串");
    }

    std::printf("\nTab 补全切分：最后一个反斜杠之后才是前缀\n");
    {
        auto s = nav::SplitForComplete(L"C:\\Windows\\Sys");
        Check(s.completable, "盘符路径可补全");
        Eq(s.dir, L"C:\\Windows\\", "目录部分带尾反斜杠");
        Eq(s.prefix, L"Sys", "前缀是最后一段");

        // 【关键回归】盘根：以前 ParentOf 返回空 → 补全整条死掉
        s = nav::SplitForComplete(L"D:\\");
        Check(s.completable, "盘根可补全（以前是死键）");
        Eq(s.dir, L"D:\\", "盘根的目录部分就是自己");
        Eq(s.prefix, L"", "盘根没有前缀，列全部");

        // 【关键回归】UNC 半截：用户敲共享名的场景
        s = nav::SplitForComplete(L"\\\\srv\\sh");
        Check(s.completable, "UNC 半截可补全（以前是死键）");
        Eq(s.dir, L"\\\\srv\\", "UNC 服务器下的目录部分");
        Eq(s.prefix, L"sh", "共享名前缀");

        s = nav::SplitForComplete(L"Windows");
        Eq(s.dir, L"", "只敲名字时没有目录部分（调用方用当前目录）");
        Eq(s.prefix, L"Windows", "整串都是前缀");
        Check(s.completable, "只敲名字可补全");

        s = nav::SplitForComplete(nav::kThisPc);
        Check(!s.completable, "命名空间不参与补全");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
