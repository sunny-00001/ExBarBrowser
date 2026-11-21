// Netscape 书签 HTML 的**词法与转义**层（唯一实现）。
//
// 【为什么抽出来】bookmarks.cpp（读写自家 bookmarks.html）与 bookmark_io.cpp
// （导入导出外部书签）各写过一套逐字相同的实现：HtmlEscape / HtmlUnescape /
// Attr / Tokenize / TagName / IsClosing。两份的差异只有函数名（Local / Of 后缀），
// 逻辑完全一致 —— 典型的历史复制。
//
// 【为什么不把 ParseLevel 也搬进来】那两份**已经行为分叉**，而且分叉是有意义的：
//   · bookmarks.cpp 解析时读 EXBAR_ID / EXBAR_ICON / EXBAR_FLAGS（保留稳定 id）；
//   · bookmark_io.cpp 导入时**故意不读**这三个属性 —— 否则导入的条目会与现有
//     书签撞 id。现在"导入不撞 id"正是靠这个分叉成立的。
// 硬合并会让两者中必有一方出错。所以本文件只收**确定等价**的词法层，
// 语义层（ParseLevel）留在各自文件，并把"为什么这里不读 EXBAR_*"写成注释。
//
// 【契约】纯字符串处理，零 Win32 依赖 —— 因此可以直接单测。
#pragma once

#include <string>
#include <vector>

namespace bmkhtml {

struct Token {
    bool tag = false;
    std::wstring text;   // tag=true 时不含尖括号
};

// 5 个实体（& < > " '）+ nbsp，外加**数字实体**（&#39; / &#x27;）。
//
// 【为什么必须认数字实体】原注释写"Netscape 书签格式里不会出现"，这是错的：
// Chrome / Edge 导出书签时把单引号写成 &#39;（不是 &apos;）。不认它，
// 从 Chrome 导入一个叫 "Bob's" 的目录，名字会变成字面的 "Bob&#39;s"。
// 这是导入路径上最常见的一种可见损坏，而它就藏在一行"不会出现"的断言后面。
std::wstring Escape(const std::wstring& s);
std::wstring Unescape(const std::wstring& s);

// 路径 <-> file:// URL（唯一实现）。
//
// 【为什么在这一层】bookmarks.cpp 里那份实现有一个只在"导入别人写的书签"时
// 才暴露的致命缺陷：百分号解码按**宽字符**取 `%XX`（`static_cast<wchar_t>(wcstol)`），
// 于是 `%E4%B8%AD` 解成三个 U+00E4/U+00B8/U+00AD 这样的拉丁补充字符，
// 永远拼不出"中"。Chrome/Edge 导出中文路径必然转义成 UTF-8 百分号序列，
// 所以"从浏览器导入中文书签"整条路都是坏的 —— 每一次导入都把路径写坏一次。
//
// 现在的规则：
//   · 解码按 **UTF-8 字节序列**还原（`%E4%B8%AD` -> "中"）；
//   · 未转义的非 ASCII 原样保留（本工程自己写的文件就是这样，往返不损失）；
//   · UNC 双向支持：`\\srv\share\x` <-> `file://srv/share/x`，
//     同时兼容旧版写出的 `file://///srv/share/x`（剥 `file:///` 后剩 `//srv/share`）；
//   · 非 file: 协议（http: 等）原样返回 —— 它是"要保存的东西"而不是"要导航的路径"，
//     猜一个本地路径出来只会更糟。
//
// 纯字符串处理，零 Win32 依赖，可直接单测 —— 这类"看着对、跑起来错"的编解码
// 正是最该有单测的地方。
std::wstring PathToUrl(const std::wstring& path);
std::wstring UrlToPath(const std::wstring& url);

// 从一段 tag 文本里取属性值（属性名大小写不敏感、值可带双引号或不带）。
// 找不到返回空串 —— 调用方据此判断"这个属性没写"，不要用空串当"值是空"。
std::wstring Attr(const std::wstring& tag, const wchar_t* key);

// 切成 tag / 文本两种 token。纯空白文本 token 会被丢弃（书签 HTML 的缩进很多）。
std::vector<Token> Tokenize(const std::wstring& html);

// 取标签名（小写、去掉前导 '/'）。`<DT>` -> "dt"，`</DL>` -> "dl"。
std::wstring TagName(const std::wstring& tag);

// 是否闭合标签（`</DL>`）。
bool IsClosing(const std::wstring& tag);

}  // namespace bmkhtml
