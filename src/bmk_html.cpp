#include "bmk_html.h"

#include <cwctype>

#include "str.h"

namespace bmkhtml {
namespace {

inline bool IsAlnum(wchar_t c) { return iswalnum(c) != 0; }
inline bool IsSpace(wchar_t c) { return iswspace(c) != 0; }

}  // namespace

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::wstring Unescape(const std::wstring& s) {
    struct Ent {
        const wchar_t* name;
        wchar_t ch;
    };
    static const Ent kEnts[] = {{L"&amp;", L'&'},  {L"&lt;", L'<'},  {L"&gt;", L'>'},
                                {L"&quot;", L'"'}, {L"&apos;", L'\''}, {L"&nbsp;", L' '}};
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') {
            out.push_back(s[i]);
            continue;
        }
        bool done = false;
        for (const auto& e : kEnts) {
            const size_t n = wcslen(e.name);
            if (s.compare(i, n, e.name) == 0) {
                out.push_back(e.ch);
                i += n - 1;
                done = true;
                break;
            }
        }
        if (!done) out.push_back(s[i]);
    }
    return out;
}

std::wstring Attr(const std::wstring& tag, const wchar_t* key) {
    const std::wstring lower = str::Lower(tag);
    const std::wstring k = str::Lower(key);
    size_t p = 0;
    while ((p = lower.find(k, p)) != std::wstring::npos) {
        // 左边界必须是**真正的分隔符**，否则属性名会被子串命中。
        //
        // 这里以前只判 `!iswalnum(lower[p-1])` —— 而 '_' 既不是字母也不是数字，
        // 于是 `EXBAR_ICON="x"` 里的 `ICON` 会被认为合法，读 ICON 拿到 x。
        // 实测确认（脚本里跑了 DATA_HREF/EXBAR_ICON 两个用例）。
        // 现在把 '_' 也算作名字字符：属性名里不允许出现它作为左边界。
        const bool leftOk = p == 0 || (!IsAlnum(lower[p - 1]) && lower[p - 1] != L'_');
        const size_t after = p + k.size();
        if (!leftOk || after >= lower.size() || lower[after] != L'=') {
            p = after;
            continue;
        }
        size_t v = after + 1;
        if (v < tag.size() && (tag[v] == L'"' || tag[v] == L'\'')) {
            const wchar_t q = tag[v++];
            size_t end = tag.find(q, v);
            if (end == std::wstring::npos) end = tag.size();
            return tag.substr(v, end - v);
        }
        size_t end = v;
        while (end < tag.size() && !IsSpace(tag[end]) && tag[end] != L'>') ++end;
        return tag.substr(v, end - v);
    }
    return std::wstring();
}

std::vector<Token> Tokenize(const std::wstring& html) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == L'<') {
            const size_t end = html.find(L'>', i);
            if (end == std::wstring::npos) break;   // 未闭合的 '<'：丢弃尾部，不猜
            Token t;
            t.tag = true;
            t.text = html.substr(i + 1, end - i - 1);
            out.push_back(std::move(t));
            i = end + 1;
        } else {
            size_t next = html.find(L'<', i);
            if (next == std::wstring::npos) next = html.size();
            std::wstring text = html.substr(i, next - i);
            // 纯空白文本 token 没有语义（书签 HTML 缩进很多），丢掉能省一大半 token。
            if (!str::Trim(text).empty()) {
                Token t;
                t.tag = false;
                t.text = std::move(text);
                out.push_back(std::move(t));
            }
            i = next;
        }
    }
    return out;
}

std::wstring TagName(const std::wstring& tag) {
    std::wstring t = str::Trim(tag);
    if (!t.empty() && t[0] == L'/') t = t.substr(1);
    size_t end = 0;
    while (end < t.size() && (IsAlnum(t[end]) || t[end] == L'!' || t[end] == L'-')) ++end;
    return str::Lower(t.substr(0, end));
}

bool IsClosing(const std::wstring& tag) {
    const std::wstring t = str::Trim(tag);
    return !t.empty() && t[0] == L'/';
}

}  // namespace bmkhtml
