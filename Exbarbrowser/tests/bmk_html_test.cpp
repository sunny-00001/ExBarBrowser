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

    printf("\nPathToUrl / UrlToPath：往返一致 + UTF-8 百分号解码 + UNC\n");
    {
        using bmkhtml::PathToUrl;
        using bmkhtml::UrlToPath;

        // 基本往返
        CHECK(UrlToPath(PathToUrl(L"D:\\a\\b")) == L"D:\\a\\b", "普通路径往返");
        CHECK(PathToUrl(L"D:\\a\\b") == L"file:///D:/a/b", "反斜杠转正斜杠 + file:///");
        // 中文：本工程自己写出的文件里中文是**原样**的，必须能读回来
        CHECK(UrlToPath(L"file:///D:/中文/目录") == L"D:\\中文\\目录",
              "未转义中文原样保留");
        CHECK(UrlToPath(PathToUrl(L"D:\\中文")) == L"D:\\中文", "中文路径往返");

        // 【核心】Chrome/Edge 导出的转义形式：%XX 必须按 UTF-8 字节解，不能按宽字符解。
        // 旧实现把 %E4 %B8 %AD 解成三个拉丁字符（"ä¸"），"中"永久丢失。
        CHECK(UrlToPath(L"file:///D:/%E4%B8%AD%E6%96%87") == L"D:\\中文",
              "UTF-8 百分号解码出中文（旧实现此处必错）");
        CHECK(UrlToPath(L"file:///C:/a%20b") == L"C:\\a b", "%20 -> 空格");
        CHECK(UrlToPath(L"file:///C:/100%25") == L"C:\\100%", "%25 -> 字面百分号");

        // 命名空间：不是文件系统路径，不能被包成 file:///
        const std::wstring kThisPc = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
        CHECK(PathToUrl(kThisPc) == kThisPc, "命名空间原样返回");
        CHECK(PathToUrl(L"shell:AppsFolder") == L"shell:AppsFolder", "shell: 原样返回");

        // UNC 双向
        CHECK(PathToUrl(L"\\\\srv\\share\\x") == L"file://srv/share/x",
              "UNC -> file://server/...（两个斜杠）");
        CHECK(UrlToPath(L"file://srv/share/x") == L"\\\\srv\\share\\x",
              "file://server/... -> UNC");
        // 兼容旧版写出的 file://///srv/share
        CHECK(UrlToPath(L"file://///srv/share/x") == L"\\\\srv\\share\\x",
              "兼容旧版多斜杠 UNC 写法");
        CHECK(UrlToPath(PathToUrl(L"\\\\srv\\share\\x")) == L"\\\\srv\\share\\x",
              "UNC 往返");

        // 非 file: 协议原样返回（网页书签不该被猜成本地路径）
        CHECK(UrlToPath(L"https://example.com/a") == L"https://example.com/a",
              "http(s) 原样返回");
        // 坏字节：宁可失败（空串）也不要写回一串替换字符
        CHECK(UrlToPath(L"file:///C:/%FF").empty(), "非法 UTF-8 序列 -> 空（拒绝写坏路径）");
        // '#' '?' 会被转义，否则当成 fragment/query 截断路径
        CHECK(UrlToPath(PathToUrl(L"D:\\a#b?c")) == L"D:\\a#b?c", "'#' '?' 往返不丢");
    }

    printf("\nUnescape：数字实体（Chrome 把 ' 写成 &#39;）\n");
    {
        CHECK(bmkhtml::Unescape(L"Bob&#39;s") == L"Bob's", "&#39; -> 单引号（Chrome 导出格式）");
        CHECK(bmkhtml::Unescape(L"&#x27;") == L"'", "十六进制数字实体");
        CHECK(bmkhtml::Unescape(L"&#20013;") == L"中", "十进制 BMP 码点");
        CHECK(bmkhtml::Unescape(L"&#128512;") == L"😀" ||
                  bmkhtml::Unescape(L"&#128512;").size() == 2, "超过 BMP 的码点写成代理对");
        CHECK(bmkhtml::Unescape(L"&#;") == L"&#;", "空数字实体原样保留");
        CHECK(bmkhtml::Unescape(L"&#xZZ;") == L"&#xZZ;", "非数字原样保留");
        CHECK(bmkhtml::Unescape(L"&#0;") == L"&#0;", "码点 0 原样保留（不写 NUL 进字符串）");
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
