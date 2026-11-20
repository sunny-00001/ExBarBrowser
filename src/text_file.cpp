#include "text_file.h"

#include <windows.h>

#include <cstring>   // memcpy（BOM 分支按字节拷贝，避免未对齐的 wchar_t 读）
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
    // ToExtended：绕过 MAX_PATH(260)。深路径以前在这里直接返回 false，
    // 而调用方把它当成"文件不存在/读不出"。
    HANDLE h = CreateFileW(ToExtended(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
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
    if (!out) return false;
    std::string bytes;
    if (!ReadAllBytes(path, &bytes)) return false;

    // BOM 分派：三种都要认。以前只认 UTF-8 与 UTF-16LE，UTF-16BE（FE FF）会掉进
    // UTF-8 分支变成整篇乱码 —— 而 Windows 记事本的"Unicode big endian"就是它。
    if (bytes.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(bytes[0]);
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xFF && b1 == 0xFE) {  // UTF-16 LE
            const size_t chars = (bytes.size() - 2) / 2;
            out->assign(chars, L'\0');
            // memcpy 而不是 reinterpret_cast：+2 是奇数偏移时未对齐读在 ARM64 上会崩。
            if (chars) memcpy(&(*out)[0], bytes.data() + 2, chars * sizeof(wchar_t));
            return true;
        }
        if (b0 == 0xFE && b1 == 0xFF) {  // UTF-16 BE：逐 16 位翻转
            const size_t chars = (bytes.size() - 2) / 2;
            out->assign(chars, L'\0');
            for (size_t i = 0; i < chars; ++i) {
                const auto hi = static_cast<unsigned char>(bytes[2 + i * 2]);
                const auto lo = static_cast<unsigned char>(bytes[3 + i * 2]);
                (*out)[i] = static_cast<wchar_t>((hi << 8) | lo);
            }
            return true;
        }
    }

    size_t skip = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        skip = 3;

    // 严格解码：非法 UTF-8 字节序列直接失败，而不是替换成 U+FFFD。
    //
    // 这一步是数据完整性的关键。宽松解码会把 GBK 的"中文"解成 4 个 U+FFFD（实测），
    // 中文名全变 "????"，调用方毫不知情；配合"加载失败当成空书签"就是永久损坏。
    // 读用户文件宁可失败（由 BookmarkStore 的护栏拒绝写盘、由调用方提示），
    // 也不能悄悄改坏它。
    if (!str::FromUtf8Strict(bytes.substr(skip), out)) {
        out->clear();
        return false;
    }
    return true;
}

bool WriteText(const std::wstring& path, const std::wstring& text) {
    // 原子写：先写 <path>.<pid>.tmp，FlushFileBuffers，再 MoveFileEx 替换。
    //
    // 三处都是踩过的坑：
    // ① **tmp 名必须带 pid**。identity.h 的 kMultiInstanceEnv 故意允许双实例；
    //    固定 ".tmp" 时 A 与 B 会共用同一个临时文件 —— B 的 CREATE_ALWAYS 截断
    //    A 正在写的内容，A 的 MoveFileEx 就把 B 的半截数据改名上位。更糟的是
    //    一方失败后 DeleteFileW(tmp) 会删掉对方正在写的文件。
    // ② **必须 FlushFileBuffers**。WriteFile 只进系统缓存就改名，NTFS 日志只保证
    //    元数据顺序；断电后可能出现"文件名已换、内容是 0 字节"。这条链上
    //    settings.json / bookmarks.html / keymap.json / ai-token.txt 全都要走。
    // ③ MoveFileEx 加 MOVEFILE_WRITE_THROUGH，让改名本身也落盘。
    std::string bytes = ToUtf8(text);
    wchar_t suffix[32] = L"";
    swprintf(suffix, 32, L".%lu.tmp", static_cast<unsigned long>(GetCurrentProcessId()));
    const std::wstring tmp = path + suffix;

    HANDLE h = CreateFileW(ToExtended(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = bytes.empty() ||
              (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                         nullptr) != FALSE &&
               written == bytes.size());  // 短写（磁盘满）=失败，防半截文件被当成功替换
    if (ok) ok = FlushFileBuffers(h) != FALSE;   // 落盘后再改名，否则断电得到空文件
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(ToExtended(tmp).c_str());
        return false;
    }
    if (!MoveFileExW(ToExtended(tmp).c_str(), ToExtended(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(ToExtended(tmp).c_str());
        return false;
    }
    return true;
}


std::wstring ToExtended(const std::wstring& path) {
    if (path.size() < 2) return path;

    // 幂等：已经加过前缀的直接返回，调用方可以无脑套。
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0) return path;

    // 命名空间（shell:、::{GUID}、搜索: 之类）不是文件系统路径 ——
    // 加 \\?\ 前缀会让它彻底打不开，所以原样放过。
    if (path.rfind(L"::", 0) == 0) return path;
    const size_t colon = path.find(L':');
    if (colon != std::wstring::npos && colon > 0) {
        // "C:\..." 是盘符（colon==1 且后面跟分隔符）；其余 "xx:yyy" 视作命名空间。
        const bool driveLetter =
            (colon == 1) && (path.size() == 2 || path[2] == L'\\' || path[2] == L'/');
        if (!driveLetter) return path;
    }

    // 相对路径（不以盘符、也不以分隔符开头）：扩展前缀要求绝对路径，原样返回。
    const bool absolute = (colon == 1) || path[0] == L'\\' || path[0] == L'/';
    if (!absolute) return path;

    // 【必须把 / 换成 \】扩展长度路径**不做任何规范化** —— 这正是它绕开
    // MAX_PATH 与 . / .. 处理的原因，也意味着正斜杠不会被自动转换。
    // 而本工程的路径来源混合（SHGetKnownFolderPath 给反斜杠，用户输入/配置
    // 里可能有正斜杠），不转换就会得到"加了前缀却打不开"的路径。
    std::wstring p = path;
    for (auto& c : p)
        if (c == L'/') c = L'\\';

    // UNC：\\server\share  ->  \\?\UNC\server\share
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);

    return L"\\\\?\\" + p;
}

bool AppendLine(const std::wstring& path, const std::string& bytes,
                unsigned long long rotateAtBytes, int checkEveryN) {
    // 轮转：每 checkEveryN 次调用查一次大小，超阈值改名为 <path>.1（只留一份）。
    //
    // 用函数内 static 计数（不是成员）：本函数是纯 IO 工具，不知道调用方是谁；
    // 每个 path 各自的"查了几次"并不重要 —— 差几次不影响正确性，只是检查频率。
    static int sinceCheck = 0;
    if (checkEveryN > 0 && ++sinceCheck >= checkEveryN) {
        sinceCheck = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(ToExtended(path).c_str(), GetFileExInfoStandard, &fad) &&
            ((static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) >
                rotateAtBytes) {
            const std::wstring old = path + L".1";
            MoveFileExW(ToExtended(path).c_str(), ToExtended(old).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        }
    }

    HANDLE h = CreateFileW(ToExtended(path).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                               nullptr) != FALSE &&
                     written == bytes.size());
    CloseHandle(h);
    return ok;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(ToExtended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(ToExtended(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(ToExtended(path).c_str());
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
