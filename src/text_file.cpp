#include "text_file.h"

#include <windows.h>

#include <vector>

#include "str.h"

namespace tf {

// UTF-8 互转的唯一实现在 str.cpp：以前 text_file / ai_api / ai_server / commands
// 各有一份 WideCharToMultiByte 样板，边界条件（空串、非法序列）处理不一致。
std::string ToUtf8(const std::wstring& s) { return str::ToUtf8(s); }

std::wstring FromUtf8(const std::string& s) { return str::FromUtf8(s); }

bool ReadAllBytes(const std::wstring& path, std::string* out) {
    if (!out) return false;
    out->clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = true;
    if (!out->empty())
        ok = ReadFile(h, &(*out)[0], static_cast<DWORD>(out->size()), &read, nullptr) != FALSE &&
             read == out->size();  // 短读=文件被并发截断，视为失败不交半截数据
    CloseHandle(h);
    if (!ok) out->clear();
    return ok;
}

bool ReadText(const std::wstring& path, std::wstring* out) {
    std::string bytes;
    if (!ReadAllBytes(path, &bytes)) return false;
    // UTF-16 LE BOM
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        size_t chars = (bytes.size() - 2) / 2;
        out->assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), chars);
        return true;
    }
    // UTF-8 BOM
    size_t skip = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        skip = 3;
    *out = FromUtf8(bytes.substr(skip));
    return true;
}

bool WriteText(const std::wstring& path, const std::wstring& text) {
    // 原子写：先写 <path>.tmp，再 MoveFileEx 替换。避免崩溃/断电留下半截配置。
    std::string bytes = ToUtf8(text);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = bytes.empty() ||
              (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                         nullptr) != FALSE &&
               written == bytes.size());  // 短写（磁盘满）=失败，防半截文件被当成功替换
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring parent = path;
    size_t slash = parent.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDir(parent.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE;
}

bool CopyFileIfMissing(const std::wstring& from, const std::wstring& to) {
    if (Exists(to)) return true;
    if (!Exists(from)) return false;
    return CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE;
}


std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(_countof(buf)));
    if (n == 0) return std::wstring();
    std::wstring s(buf, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t tail = a.back();
    if (tail == L'\\' || tail == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace tf
