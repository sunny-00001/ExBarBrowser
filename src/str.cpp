// 字符串工具实现（唯一）。见 str.h 的收编说明。
#include "str.h"

#include <windows.h>

#include <cwctype>

namespace str {

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

int ICmp(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str());
}

bool IStartsWith(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n, nullptr,
                        nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::wstring FromUtf8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

bool FromUtf8Strict(const std::string& s, std::wstring* out) {
    if (!out) return false;
    out->clear();
    if (s.empty()) return true;

    // MB_ERR_INVALID_CHARS：遇到非法序列直接失败（GetLastError = ERROR_NO_UNICODE_TRANSLATION
    // = 1113），而不是替换成 U+FFFD。宽松版在这里会把 GBK 字节静默解成 "????"。
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    out->assign(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(),
                            static_cast<int>(s.size()), &(*out)[0], n) != n) {
        out->clear();
        return false;
    }
    return true;
}

}  // namespace str
