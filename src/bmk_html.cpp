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
        // 数字实体 &#NN; / &#xHH;。Chrome/Edge 把单引号写成 &#39;，
        // 不认它就会把 "Bob's" 导入成 "Bob&#39;s"（见头文件说明）。
        if (!done && i + 1 < s.size() && s[i + 1] == L'#') {
            size_t j = i + 2;
            bool hex = false;
            if (j < s.size() && (s[j] == L'x' || s[j] == L'X')) {
                hex = true;
                ++j;
            }
            const size_t start = j;
            long v = 0;
            while (j < s.size() && j - start < 6) {
                const wchar_t c = s[j];
                int digit = -1;
                if (c >= L'0' && c <= L'9') digit = c - L'0';
                else if (hex && c >= L'a' && c <= L'f') digit = c - L'a' + 10;
                else if (hex && c >= L'A' && c <= L'F') digit = c - L'A' + 10;
                if (digit < 0) break;
                v = v * (hex ? 16 : 10) + digit;
                ++j;
            }
            if (j > start && j < s.size() && s[j] == L';' && v > 0 && v <= 0x10FFFF) {
                // 超出 BMP 的码点写成代理对。实体本身合法但值是 0 或超过 Unicode
                // 上界时原样保留 —— 猜一个字符出来只会把坏数据洗白。
                if (v <= 0xFFFF) {
                    out.push_back(static_cast<wchar_t>(v));
                } else {
                    const unsigned cp = static_cast<unsigned>(v) - 0x10000;
                    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
                i = j;   // 跳过 ';'
                done = true;
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

// ---------------------------------------------------------------- 路径 <-> URL
//
// 见头文件的说明：旧实现按宽字符解 %XX，中文永远解不出来。

namespace {

inline int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// 需要转义的字符：会破坏 URL/HTML 属性往返的那几个。
//
// 刻意**不**把非 ASCII 转义：本工程自己的 bookmarks.html 里中文是原样的，
// 转成 %E4%B8%AD 虽然更"标准"，但会让每次启动首屏都要解一遍、也让用户手工
// 编辑文件时看不懂。Chrome 的转义在**读**的时候已经被正确还原了（这是本条的目的），
// 写回去时保持我们自己的风格即可。
bool NeedsEscape(wchar_t c) {
    if (c < 0x21 || c == 0x7F) return true;   // 控制符 + 空格
    return c == L'%' || c == L'#' || c == L'?' || c == L'"' || c == L'<' || c == L'>';
}

// 一段 UTF-8 字节还原成宽字符。非法序列整体放弃（返回空），由调用方原样保留
// 源文本 —— 宁可留着 %XX 让人看得出"这里没解对"，也不要写进一串半截字符。
std::wstring Utf8BytesToWide(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return std::wstring();
    std::wstring out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        const unsigned char b = bytes[i];
        int extra = 0;
        unsigned cp = 0;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            extra = 1;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            extra = 2;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            extra = 3;
            cp = b & 0x07;
        } else {
            return std::wstring();   // 0x80..0xBF 不能当首字节 / 0xF8+ 非法
        }
        if (extra > 0 && i + static_cast<size_t>(extra) >= bytes.size())
            return std::wstring();   // 序列被截断：整体放弃
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cb = bytes[i + static_cast<size_t>(k)];
            if ((cb & 0xC0) != 0x80) return std::wstring();
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return std::wstring();          // 过长编码
        if (extra == 2 && cp < 0x800) return std::wstring();
        if (extra == 3 && cp < 0x10000) return std::wstring();
        if (cp > 0x10FFFF) return std::wstring();
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            const unsigned v = cp - 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

}  // namespace

std::wstring PathToUrl(const std::wstring& path) {
    if (path.empty()) return path;
    // 命名空间（::{GUID} / shell:xxx）不是文件系统路径，套 file:// 会让它打不开。
    if (path.rfind(L"::", 0) == 0 || path.rfind(L"shell:", 0) == 0) return path;
    // 已经是 URL 的（极少见，但导入导出会碰到）原样返回，不重复包一层。
    if (path.find(L"://") != std::wstring::npos) return path;

    std::wstring s = path;
    for (auto& c : s)
        if (c == L'\\') c = L'/';

    // UNC：\\server\share\x -> file://server/share/x（**两个**斜杠）。
    // 写成 file:///server/... 会被解析成"根目录下有个叫 server 的文件夹"。
    bool unc = false;
    if (s.rfind(L"//", 0) == 0) {
        unc = true;
        s = s.substr(2);
    }

    std::wstring out = unc ? L"file://" : L"file:///";
    for (wchar_t c : s) {
        if (!NeedsEscape(c)) {
            out.push_back(c);
        } else {
            wchar_t buf[8];
            swprintf(buf, 8, L"%%%02X", static_cast<unsigned>(c) & 0xFF);
            out += buf;
        }
    }
    return out;
}

std::wstring UrlToPath(const std::wstring& url) {
    if (url.empty()) return url;
    std::wstring s = url;

    bool unc = false;
    if (s.rfind(L"file:///", 0) == 0) {
        s = s.substr(8);
        // 旧版把 UNC 写成 file://///srv/share（剥掉 file:/// 后剩 //srv/share）。
        // 兼容它 —— 用户机器上可能已经存着这样写出来的书签。
        if (s.rfind(L"//", 0) == 0) {
            unc = true;
            s = s.substr(2);
            if (s.rfind(L"/", 0) == 0) s = s.substr(1);   // 第三根斜杠
        }
    } else if (s.rfind(L"file://", 0) == 0) {
        s = s.substr(7);
        unc = true;   // file://server/share —— 文件协议下这只能是 UNC
    } else {
        // http: / https: / ftp: 等：原样返回。它不是可导航的本地路径，
        // 猜一个出来（比如 "http:\\..."）只会让"导入的网页书签"变成坏路径。
        return url;
    }

    // 百分号解码：**先按 UTF-8 字节收集，再整体还原**。
    //
    // 这是与旧实现的唯一实质差别。旧实现 `out.push_back(wcstol(hex))` 把每个
    // %XX 单独当成一个宽字符，于是 UTF-8 的多字节序列被拆成几个拉丁字符：
    //   %E4%B8%AD -> L'\xE4' L'\xB8' L'\xAD'（"ä¸"），"中"永久丢失。
    std::vector<unsigned char> bytes;
    bytes.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int hi = HexVal(s[i + 1]);
            const int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<unsigned char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        // 非转义字符：按 UTF-8 编码压进去，与转义出来的字节同一条还原路径。
        // （本工程自己写出的文件里中文就是原样的，必须能正确走完。）
        const wchar_t c = s[i];
        if (c < 0x80) {
            bytes.push_back(static_cast<unsigned char>(c));
        } else if (c <= 0x7FF) {
            bytes.push_back(static_cast<unsigned char>(0xC0 | (c >> 6)));
            bytes.push_back(static_cast<unsigned char>(0x80 | (c & 0x3F)));
        } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
            const wchar_t lo2 = s[i + 1];
            if (lo2 >= 0xDC00 && lo2 <= 0xDFFF) {
                const unsigned cp = 0x10000 +
                                    ((static_cast<unsigned>(c) - 0xD800) << 10) +
                                    (static_cast<unsigned>(lo2) - 0xDC00);
                bytes.push_back(static_cast<unsigned char>(0xF0 | (cp >> 18)));
                bytes.push_back(static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F)));
                bytes.push_back(static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)));
                bytes.push_back(static_cast<unsigned char>(0x80 | (cp & 0x3F)));
                ++i;
                continue;
            }
            bytes.push_back(static_cast<unsigned char>(0xEF));  // 孤立代理：U+FFFD
            bytes.push_back(static_cast<unsigned char>(0xBF));
            bytes.push_back(static_cast<unsigned char>(0xBD));
        } else {
            bytes.push_back(static_cast<unsigned char>(0xE0 | (c >> 12)));
            bytes.push_back(static_cast<unsigned char>(0x80 | ((c >> 6) & 0x3F)));
            bytes.push_back(static_cast<unsigned char>(0x80 | (c & 0x3F)));
        }
    }

    std::wstring decoded = Utf8BytesToWide(bytes);
    // 解不出来（坏字节序列）时返回空串：调用方（BookmarkStore）据此丢弃该节点，
    // 而不是把一串替换字符写进用户的路径。这与"读用户文件宁可失败"同一口径。
    if (decoded.empty() && !bytes.empty()) return std::wstring();

    for (auto& c : decoded)
        if (c == L'/') c = L'\\';

    if (unc) {
        // file://server/share -> \\server\share
        decoded = L"\\\\" + decoded;
    }
    return decoded;
}

}  // namespace bmkhtml
