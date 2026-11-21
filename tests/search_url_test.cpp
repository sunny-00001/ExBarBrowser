// search-ms URL 拼装自测。
//
// 守的是两类问题：
//   1. **引号不转义** —— 搜索词里的 " 会让 search-ms 提前结束短语，结果莫名其妙。
//      这段逻辑原来长在导航栏回调里，改错了没有任何测试会红。
//   2. **"只打了空格"被当成搜索词** —— 搜索框提交不做 trim，用户敲了几个空格再回车
//      就进入一个搜不到任何东西的搜索视图，看起来像"搜索坏了"。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -target x86_64-w64-mingw32 -Isrc \
//     tests/search_url_test.cpp src/search_url.cpp -o /tmp/search_url_test.exe
#include <cstdio>
#include <string>

#include "search_url.h"

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

static void Dump(const char* what, const std::wstring& s) {
    printf("     %s = '%ls'\n", what, s.c_str());
}

int main() {
    printf("Trim：只吃首尾空白，不动中间与全角\n");
    {
        CHECK(searchurl::Trim(L"  abc  ") == L"abc", "首尾空格被去掉");
        CHECK(searchurl::Trim(L"\tabc\t") == L"abc", "首尾制表符被去掉");
        CHECK(searchurl::Trim(L"   ") == L"", "全是空格 -> 空串（=取消搜索）");
        CHECK(searchurl::Trim(L"") == L"", "空串仍是空串");
        CHECK(searchurl::Trim(L"a b") == L"a b", "中间空格保留（短语搜索要靠它）");
        // 全角空格是**字符**不是分隔：用户可能真想搜它。
        CHECK(searchurl::Trim(L"\u3000a\u3000") == L"\u3000a\u3000",
              "全角空格不当空白吃掉");
    }

    printf("\nBuild：结构、转义、范围原样\n");
    {
        CHECK(searchurl::Build(L"", L"C:\\dir").empty(),
              "空查询 -> 空 URL（调用方据此退出搜索）");

        const std::wstring u = searchurl::Build(L"abc", L"C:\\dir");
        Dump("url", u);
        CHECK(u == L"search-ms:query=abc&crumb=location:C:\\dir&",
              "结构与原实现逐字一致（改拼接方式会让现在能用的路径搜不出东西）");

        const std::wstring q = searchurl::Build(L"say \"hi\"", L"C:\\dir");
        Dump("引号", q);
        CHECK(q.find(L"say \"\"hi\"\"") != std::wstring::npos,
              "搜索词里的双引号翻倍转义");

        // 范围原样拼进去：不转义、不加引号。这是现状行为（已知局限：
        // 路径含 & 会被当成参数分隔符），固定成契约，要改就是一次有测试兜底的改动。
        const std::wstring s = searchurl::Build(L"x", L"D:\\a b\\中文");
        Dump("范围", s);
        CHECK(s.find(L"crumb=location:D:\\a b\\中文&") != std::wstring::npos,
              "范围原样进入 crumb=location:（空格与中文都不转义）");

        CHECK(searchurl::Build(L"a b", L"C:\\d") ==
                  L"search-ms:query=a b&crumb=location:C:\\d&",
              "查询词内部空格保留");
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
