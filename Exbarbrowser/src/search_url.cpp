// 见 search_url.h。
#include "search_url.h"

namespace searchurl {

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    // 只吃 ASCII 空白：全角空格（U+3000）可能是用户真想搜的字符，不当分隔。
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(b, e - b);
}

namespace {

// 搜索词里的双引号要翻倍转义：search-ms 用它包短语，不转义会把短语提前截断
// （搜 "a" b" 这种词时结果莫名其妙）。
//
// 单独抽出来是因为拼装与单测都要用同一份转义 —— 抄一份就会出现
// "某条路不转义"的静默分叉。
std::wstring EscapeQuery(const std::wstring& query) {
    std::wstring q = query;
    for (size_t i = 0; i < q.size(); ++i)
        if (q[i] == L'"') q.insert(i++, 1, L'"');
    return q;
}

}  // namespace

std::wstring Build(const std::wstring& query, const std::wstring& scope) {
    if (query.empty()) return std::wstring();

    // crumb 限定范围，query 是搜索词。末尾那个 & 是原实现的写法，保留。
    return L"search-ms:query=" + EscapeQuery(query) + L"&crumb=location:" + scope + L"&";
}

}  // namespace searchurl