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

// 5 个实体（& < > " '）+ nbsp。注意：只认这 6 个具名实体，
// 不处理数字实体（&#xxxx;）—— Netscape 书签格式里不会出现。
std::wstring Escape(const std::wstring& s);
std::wstring Unescape(const std::wstring& s);

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
