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

    printf(fails ? "\nFAILED %d\n" : "\nPASS\n", fails);
    return fails ? 1 : 0;
}
