// Netscape 书签 HTML 词法层自测：bmkhtml::Escape/Unescape/Attr/Tokenize/TagName。
//
// 这一层原先在 bookmarks.cpp 与 bookmark_io.cpp 各有一份逐字相同的副本，
// 且**零测试**。收编时最怕的是"合并时把某一方的细微行为弄丢"，
// 所以这里把边界逐条钉住 —— 特别是 Attr 的"属性名被子串误命中"这一类。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/bmk_html_test.cpp src/bmk_html.cpp src/str.cpp \
//     -o /tmp/bmk_html_test.exe -lole32 -luuid -lshell32
#include <cstdio>
#include <string>

#include "bmk_html.h"

static int fails = 0;
#define CHECK(cond, msg)          \
    do {                          \
        if (cond) {               \
            printf("ok   %s\n", msg);     \
        } else {                  \
            printf("FAIL %s\n", msg);     \
            ++fails;              \
        }                         \
    } while (0)

int main() {
    printf("转义/反转义\n");
    {
        CHECK(bmkhtml::Escape(L"a&b<c>d\"e") == L"a&amp;b&lt;c&gt;d&quot;e", "四个特殊字符都转义");
        CHECK(bmkhtml::Escape(L"中文路径") == L"中文路径", "非 ASCII 不动");
        CHECK(bmkhtml::Unescape(L"a&amp;b&lt;c&gt;d&quot;e") == L"a&b<c>d\"e", "反转义往返");
        CHECK(bmkhtml::Unescape(L"&apos;&nbsp;") == L"'\xA0" || bmkhtml::Unescape(L"&apos;&nbsp;") == L"' ",
              "apos / nbsp 被解");
        CHECK(bmkhtml::Unescape(L"裸 & 与 &unknown;") == L"裸 & 与 &unknown;", "未知实体原样保留");
        // 往返
        const std::wstring raw = L"C:\\a&b\\<x>\\\"q\".txt";
        CHECK(bmkhtml::Unescape(bmkhtml::Escape(raw)) == raw, "任意路径往返一致");
    }

    printf("\nAttr：大小写不敏感、引号可省、左侧边界\n");
    {
        CHECK(bmkhtml::Attr(L"HREF=\"file:///D:/\" EXBAR_ID=\"b-1\"", L"href") == L"file:///D:/",
              "双引号值");
        CHECK(bmkhtml::Attr(L"HREF='x'", L"HREF") == L"x", "单引号值");
        CHECK(bmkhtml::Attr(L"HREF=x", L"href") == L"x", "无引号值（到空白为止）");
        CHECK(bmkhtml::Attr(L"href=\"a\" HREF=\"b\"", L"HREF") == L"a", "重复属性取第一个");

        // 关键边界：属性名不能被子串误命中
        CHECK(bmkhtml::Attr(L"DATA_HREF=\"bad\"", L"HREF").empty(),
              "DATA_HREF 里的 HREF 不算（左边界非字母数字）");
        CHECK(bmkhtml::Attr(L"EXBAR_ICON=\"x\"", L"ICON").empty(),
              "EXBAR_ICON 里的 ICON 不算");
        CHECK(bmkhtml::Attr(L"ICON=\"x\"", L"ICON") == L"x", "独立 ICON 正常取到");

        CHECK(bmkhtml::Attr(L"A=1", L"B").empty(), "属性不存在返回空");
        CHECK(bmkhtml::Attr(L"HREF=\"unterminated", L"href") == L"unterminated",
              "引号未闭合时取到串尾（不崩）");
    }

    printf("\nTokenize：tag/文本切分、空白文本丢弃\n");
    {
        const auto t = bmkhtml::Tokenize(L"<DL><p>\r\n  <DT><A HREF=\"x\">名字</A>\r\n</DL>");
        int tags = 0, texts = 0;
        for (const auto& tk : t) (tk.tag ? tags : texts)++;
        CHECK(tags == 6, "6 个 tag（DL / p / DT / A / /A / /DL）");
        CHECK(texts == 1, "只保留 1 个非空白文本 token（名字）");

        bool hasName = false;
        for (const auto& tk : t)
            if (!tk.tag && tk.text == L"名字") hasName = true;
        CHECK(hasName, "文本内容正确");

        CHECK(bmkhtml::Tokenize(L"").empty(), "空输入 -> 空");
        CHECK(bmkhtml::Tokenize(L"没有标签的纯文本").size() == 1, "纯文本 1 个 token");
        // 未闭合的 '<' 之后：'<' 起点被丢弃，但之前的文本 token 保留。
        // 契约是"不猜未闭合标签的内容"，不是"整段丢弃"。
        const auto t2 = bmkhtml::Tokenize(L"<DL>尾巴<未闭合");
        CHECK(t2.size() == 2 && t2[1].text == L"尾巴", "未闭合 '<' 丢弃其后的标签，保留之前的文本");
    }

    printf("\nTagName / IsClosing\n");
    {
        CHECK(bmkhtml::TagName(L"DL") == L"dl", "大写转小写");
        CHECK(bmkhtml::TagName(L" /DL ") == L"dl", "闭合标签去 '/' 与空白");
        CHECK(bmkhtml::TagName(L"A HREF=\"x\"") == L"a", "带属性只取名字");
        CHECK(bmkhtml::TagName(L"!--comment") == L"!--comment", "注释标签（- 与 ! 算名字字符）");

        CHECK(bmkhtml::IsClosing(L"/DL"), "IsClosing 认闭合");
        CHECK(!bmkhtml::IsClosing(L"DL"), "IsClosing 不误判开始标签");
        CHECK(!bmkhtml::IsClosing(L"  "), "空白不算闭合");
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
