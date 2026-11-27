// 路径分隔符折叠自测。
//
// 守的是一个**实测过的**失败：
//   SHCreateItemFromParsingName 对连续分隔符返回 E_INVALIDARG(0x80070057)。
//   实测 `C:\Windows` / `C:/Windows` 正常，`C://Windows` / `C:\\Windows` 失败。
// 这段折叠逻辑原来长在导航路径处理里，改错了没有任何测试会红 ——
// 而且症状是"导航到某个目录莫名失败"，极难定位。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -target x86_64-w64-mingw32 -Isrc \
//     tests/path_util_test.cpp src/path_util.cpp -o /tmp/path_util_test.exe
#include <cstdio>
#include <string>

#include "path_util.h"

static int fails = 0;
#define CHECK(cond, msg)                  \
    do {                                  \
        if (cond) {                       \
            printf("ok   %s\n", msg);     \
        } else {                          \
            printf("FAIL %s\n", msg);     \
            ++fails;                      \
        }                                 \
    } while (0)

int main() {
    printf("CollapseSeparators：折叠连续分隔符，保留 UNC 前缀\n");

    // ---- 普通盘符路径 ----
    CHECK(pathutil::CollapseSeparators(L"C:\\Windows") == L"C:\\Windows",
          "已经是单分隔符的原样返回");
    CHECK(pathutil::CollapseSeparators(L"C:\\\\Windows") == L"C:\\Windows",
          "中间的双反斜杠被折成一个");
    CHECK(pathutil::CollapseSeparators(L"C:\\\\\\\\Windows") == L"C:\\Windows",
          "四个反斜杠也折成一个");
    CHECK(pathutil::CollapseSeparators(L"C:\\Windows\\\\System32") == L"C:\\Windows\\System32",
          "两处连续都折");
    CHECK(pathutil::CollapseSeparators(L"C:\\") == L"C:\\",
          "盘根的单反斜杠不动");
    CHECK(pathutil::CollapseSeparators(L"C:\\\\") == L"C:\\",
          "盘根的双反斜杠折成单");

    // ---- UNC 前缀必须保留 ----
    CHECK(pathutil::CollapseSeparators(L"\\\\server\\share") == L"\\\\server\\share",
          "UNC 前缀双反斜杠原样保留");
    CHECK(pathutil::CollapseSeparators(L"\\\\\\\\server\\share") == L"\\\\server\\share",
          "UNC 前缀之后的冗余折掉");
    CHECK(pathutil::CollapseSeparators(L"\\\\server\\share\\\\dir") == L"\\\\server\\share\\dir",
          "UNC 路径中间的连续也折");
    CHECK(pathutil::CollapseSeparators(L"\\\\") == L"\\\\",
          "纯 UNC 前缀不动");

    // ---- 单反斜杠开头不是 UNC ----
    CHECK(pathutil::CollapseSeparators(L"\\foo") == L"\\foo",
          "单反斜杠开头原样保留（是盘根下的相对路径）");
    CHECK(pathutil::CollapseSeparators(L"\\foo\\\\bar") == L"\\foo\\bar",
          "单反斜杠开头的路径内部照样折叠");

    // ---- 边界 ----
    CHECK(pathutil::CollapseSeparators(L"").empty(), "空串仍是空串");
    CHECK(pathutil::CollapseSeparators(L"Windows") == L"Windows",
          "不含分隔符的原样返回");
    CHECK(pathutil::CollapseSeparators(L"C:\\Windows\\") == L"C:\\Windows\\",
          "结尾单反斜杠保留");

    // ================================================================
    // ToShellPath：外部路径 → shell 能解析的形式
    //
    // 守的是**实测过**的失败：SHCreateItemFromParsingName 对正斜杠
    // 返回 E_INVALIDARG(0x80070057)（2026-11-21 实测）。fs.* 这一组命令
    // 以前没有归一化，于是 AI 传 "C:/dir/file.txt" 时报的是
    // "无法定位源文件（HRESULT=0x80070057）" —— 真因（分隔符）完全看不出来。
    // ================================================================
    printf("\nToShellPath：正斜杠→反斜杠、折叠连续分隔符、去尾分隔符\n");

    CHECK(pathutil::ToShellPath(L"C:\\dir\\a.txt") == L"C:\\dir\\a.txt",
          "已经是反斜杠的原样返回");
    CHECK(pathutil::ToShellPath(L"C:/dir/a.txt") == L"C:\\dir\\a.txt",
          "正斜杠全部换成反斜杠");
    CHECK(pathutil::ToShellPath(L"C://dir//a.txt") == L"C:\\dir\\a.txt",
          "正斜杠 + 连续分隔符一起归一");
    CHECK(pathutil::ToShellPath(L"C:\\dir\\a.txt") == L"C:\\dir\\a.txt",
          "连续反斜杠折叠成一个");
    CHECK(pathutil::ToShellPath(L"C:\\dir\\a.txt\\") == L"C:\\dir\\a.txt",
          "尾分隔符去掉");
    CHECK(pathutil::ToShellPath(L"C:\\") == L"C:\\",
          "盘根的尾反斜杠必须留着（去掉就变成 C: = 当前目录）");
    CHECK(pathutil::ToShellPath(L"C:\\dir\\\\") == L"C:\\dir",
          "目录尾部的多个分隔符折成一个再去掉");

    // ---- UNC：前缀必须保留 ----
    CHECK(pathutil::ToShellPath(L"\\\\server\\share\\a.txt") == L"\\\\server\\share\\a.txt",
          "UNC 路径原样保留");
    CHECK(pathutil::ToShellPath(L"//server/share/a.txt") == L"\\\\server\\share\\a.txt",
          "UNC 的正斜杠也换掉，但前导双反斜杠保住");

    // ---- 命名空间路径原样返回 ----
    CHECK(pathutil::ToShellPath(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") ==
              L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
          "::{GUID} 命名空间原样返回");
    CHECK(pathutil::ToShellPath(L"shell:Downloads") == L"shell:Downloads",
          "shell: 协议串原样返回");

    // ---- 边界 ----
    CHECK(pathutil::ToShellPath(L"").empty(), "空串仍是空串");
    CHECK(pathutil::ToShellPath(L"notapath") == L"notapath", "不含分隔符的原样返回");

    // ---- ToParsingName：命名空间解析名补 shell: 前缀 ----
    //
    // 守的是一个**实测过的**失败：SIGDN_DESKTOPABSOLUTEPARSING 对命名空间项
    // 返回裸 `::{GUID}`，而裸形式对"主页"（679F85CB-…）返回 E_INVALIDARG。
    // 实机症状是会话恢复后右窗格一直空白（[nav] parse hr=0x80070057）。
    printf("\nToParsingName：裸 ::{GUID} 补 shell: 前缀、其余原样\n");
    {
        const std::wstring home = L"::{679F85CB-0220-4080-B29B-5540CC05AAB6}";
        CHECK(pathutil::ToParsingName(home) ==
                  L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",
              "裸 ::{GUID} 补上 shell: 前缀");
        // 已带前缀必须原样 —— 再补一次会变成 shell::shell:::{…}，一样解析失败
        // 而且更难看出原因。
        const std::wstring already = L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
        CHECK(pathutil::ToParsingName(already) == already, "已带 shell: 的不重复加");
        CHECK(pathutil::ToParsingName(pathutil::ToParsingName(home)) ==
                  pathutil::ToParsingName(home),
              "归一化是幂等的");
        // 文件系统路径绝不能被套前缀：shell:C:\Windows 全线失败。
        CHECK(pathutil::ToParsingName(L"C:\\Windows") == L"C:\\Windows",
              "文件系统路径原样返回");
        CHECK(pathutil::ToParsingName(L"D:\\") == L"D:\\", "盘根原样返回");
        CHECK(pathutil::ToParsingName(L"shell:Downloads") == L"shell:Downloads",
              "shell: 别名原样返回");
        CHECK(pathutil::ToParsingName(L"").empty(), "空串原样返回空");
    }

    // ---- 两个归一化叠加：先分隔符、再命名空间前缀 ----
    printf("\n组合：导航层对会话路径同时过两个归一化\n");
    {
        // 会话里存的是裸 ::{GUID}：ToShellPath 不动它，ToParsingName 补前缀。
        const std::wstring raw = L"::{679F85CB-0220-4080-B29B-5540CC05AAB6}";
        const std::wstring nav = pathutil::ToParsingName(pathutil::ToShellPath(raw));
        CHECK(nav == L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",
              "裸命名空间经两步归一后可直接解析");
        // 文件系统路径经两步后仍不含 shell: 前缀。
        const std::wstring fs = pathutil::ToParsingName(pathutil::ToShellPath(L"C:/Windows//"));
        CHECK(fs == L"C:\\Windows", "文件系统路径经两步归一后仍是纯路径");
    }

    // ---- IsUnder：真子路径判定（锁后导航守卫的判据）----
    printf("\nIsUnder：真子路径（锁后导航只拦\"往下走\"）\n");
    {
        CHECK(pathutil::IsUnder(L"C:\\A\\B", L"C:\\A"), "直接子目录");
        CHECK(pathutil::IsUnder(L"C:\\A\\B\\C", L"C:\\A"), "隔代子目录");
        CHECK(pathutil::IsUnder(L"c:\\a\\b", L"C:\\A"), "大小写不敏感");
        // 前缀陷阱：`C:\AB` 不是 `C:\A` 的子路径。这是本函数存在的核心理由 ——
        // 只比字符串前缀的实现会把它判成子路径，于是锁定的 C:\A 会拦住 C:\AB。
        CHECK(!pathutil::IsUnder(L"C:\\AB", L"C:\\A"), "前缀陷阱：C:\\AB 不是 C:\\A 的子路径");
        CHECK(!pathutil::IsUnder(L"C:\\A", L"C:\\A"), "自身不算子路径");
        CHECK(!pathutil::IsUnder(L"C:\\A\\", L"C:\\A"), "尾部反斜杠：同一个目录，不算子路径");
        CHECK(!pathutil::IsUnder(L"C:\\A", L"C:\\A\\B"), "父不是子的子路径（方向反了）");
        CHECK(!pathutil::IsUnder(L"D:\\X", L"C:\\A"), "不同盘：不是子路径");
        // 尾部反斜杠归一：`C:\A\` 与 `C:\A` 当父时判据一致。
        CHECK(pathutil::IsUnder(L"C:\\A\\B", L"C:\\A\\"), "父带尾斜杠：仍能判出子路径");
        // 盘根作父：C:\ 去掉尾斜杠后是 C:，拼分隔符后仍能正确判。
        CHECK(pathutil::IsUnder(L"C:\\Windows", L"C:\\"), "盘根作父：直接子目录");
        CHECK(!pathutil::IsUnder(L"C:\\", L"C:\\"), "盘根自身不算自己的子路径");
        // 空串：任何一侧为空都不成立（避免"空父路径匹配一切"）。
        CHECK(!pathutil::IsUnder(L"", L"C:\\A"), "子为空：不成立");
        CHECK(!pathutil::IsUnder(L"C:\\A\\B", L""), "父为空：不成立");
        // 正斜杠也认（外部字符串可能带 /）。
        CHECK(pathutil::IsUnder(L"C:/A/B", L"C:/A"), "正斜杠分隔");
    }

    printf(fails ? "\nFAILED %d\n" : "\nPASS\n", fails);
    return fails ? 1 : 0;
}
