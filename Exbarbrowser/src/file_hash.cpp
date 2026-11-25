// 文件摘要实现（说明见 file_hash.h）。
#include "file_hash.h"

#include <windows.h>

#include <bcrypt.h>

#include <vector>

#include "str.h"
#include "text_file.h"  // tf::ToExtended：>260 字符路径需要扩展长度前缀

namespace filehash {
namespace {

bool NtOk(NTSTATUS s) { return s >= 0; }

LPCWSTR AlgoId(Algo a) {
    switch (a) {
    case Algo::Md5: return BCRYPT_MD5_ALGORITHM;
    case Algo::Sha1: return BCRYPT_SHA1_ALGORITHM;
    case Algo::Sha256: return BCRYPT_SHA256_ALGORITHM;
    }
    return BCRYPT_SHA256_ALGORITHM;
}

std::wstring WithCode(const wchar_t* what, DWORD code) {
    return std::wstring(what) + L"（错误 " + std::to_wstring(code) + L"）";
}

// BCrypt 句柄的 RAII。三条资源（算法/哈希/对象缓冲）生命周期不同，但都在本次调用内，
// 用一个小的守卫比每个 return 前手写三行 Close 更不容易漏。
struct HashCtx {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> obj;

    ~HashCtx() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

}  // namespace

const wchar_t* AlgoName(Algo a) {
    switch (a) {
    case Algo::Md5: return L"MD5";
    case Algo::Sha1: return L"SHA-1";
    case Algo::Sha256: return L"SHA-256";
    }
    return L"SHA-256";
}

bool ParseAlgo(const std::wstring& s, Algo* out) {
    const std::wstring k = str::Lower(str::Trim(s));
    // 连字符是显示形式的残留（"SHA-256"）：用户会照抄界面上的名字，
    // 这里剥掉而不是让两种写法一个有用一个没有。
    std::wstring flat;
    flat.reserve(k.size());
    for (wchar_t c : k)
        if (c != L'-' && c != L'_') flat.push_back(c);

    Algo a{};
    if (flat == L"md5") a = Algo::Md5;
    else if (flat == L"sha1") a = Algo::Sha1;
    else if (flat == L"sha256") a = Algo::Sha256;
    else return false;

    if (out) *out = a;
    return true;
}

bool HashFile(const std::wstring& path, Algo algo, Result* out,
              const std::atomic<bool>* cancel, unsigned long long budgetMs) {
    if (!out) return false;
    *out = Result{};

    const DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        out->error = L"路径不存在或无法访问";
        return false;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        out->error = L"是目录，不能计算摘要";
        return false;
    }

    // 共享读+写+删除：测一个正在被别的程序写着的日志文件不该失败。
    // 代价是可能读到正在变化的内容，但那本来就是调用方的判断问题，
    // 我们只保证"读到的字节数"如实报告。
    HANDLE f = CreateFileW(tf::ToExtended(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        out->error = WithCode(L"打开文件失败", GetLastError());
        return false;
    }

    HashCtx ctx;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&ctx.alg, AlgoId(algo), nullptr, 0);
    if (!NtOk(st)) {
        CloseHandle(f);
        out->error = L"系统不支持该摘要算法";
        return false;
    }

    DWORD objLen = 0, got = 0;
    st = BCryptGetProperty(ctx.alg, BCRYPT_OBJECT_LENGTH,
                           reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0);
    if (!NtOk(st) || objLen == 0) {
        CloseHandle(f);
        out->error = L"读取算法参数失败";
        return false;
    }
    ctx.obj.resize(objLen);

    st = BCryptCreateHash(ctx.alg, &ctx.hash, ctx.obj.data(), objLen, nullptr, 0, 0);
    if (!NtOk(st)) {
        CloseHandle(f);
        out->error = L"创建摘要上下文失败";
        return false;
    }

    unsigned char buf[64 * 1024];
    // 时间预算的截止时刻。budgetMs==0 → 不限（deadline 恒为 0，判据跳过）。
    const ULONGLONG deadline = budgetMs > 0 ? GetTickCount64() + budgetMs : 0;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            CloseHandle(f);
            out->error = L"已取消";
            // 与 timedOut 分开：取消是"不要了"，超时是"没时间"。
            // 调用方要据此决定"重试"还是"放弃"。
            out->cancelled = true;
            return false;
        }
        // 时间预算：每 64 KB 检查一次，粒度足够细（几十毫秒）。
        // 超了就把已读的字节数如实留在 out->bytes，并标 timedOut ——
        // 调用方要能区分"没算完"和"算失败"。
        if (deadline && GetTickCount64() > deadline) {
            CloseHandle(f);
            out->error = L"超出时间预算，未算完";
            out->timedOut = true;
            return false;
        }
        DWORD n = 0;
        if (!ReadFile(f, buf, sizeof(buf), &n, nullptr)) {
            const DWORD e = GetLastError();
            CloseHandle(f);
            out->error = WithCode(L"读文件失败", e);
            return false;
        }
        if (n == 0) break;  // EOF
        if (!NtOk(BCryptHashData(ctx.hash, buf, n, 0))) {
            CloseHandle(f);
            out->error = L"摘要计算失败";
            return false;
        }
        out->bytes += n;
    }
    CloseHandle(f);

    unsigned char digest[64]{};  // 最长是 SHA-512 的 64 字节，这里三种都够
    DWORD digestLen = 0;
    st = BCryptGetProperty(ctx.alg, BCRYPT_HASH_LENGTH,
                           reinterpret_cast<PUCHAR>(&digestLen), sizeof(digestLen), &got, 0);
    if (!NtOk(st) || digestLen == 0 || digestLen > sizeof(digest)) {
        out->error = L"读取摘要长度失败";
        return false;
    }
    if (!NtOk(BCryptFinishHash(ctx.hash, digest, digestLen, 0))) {
        out->error = L"摘要收尾失败";
        return false;
    }

    static const wchar_t* kHex = L"0123456789abcdef";
    out->hex.reserve(static_cast<size_t>(digestLen) * 2);
    for (DWORD i = 0; i < digestLen; ++i) {
        out->hex.push_back(kHex[digest[i] >> 4]);
        out->hex.push_back(kHex[digest[i] & 0xF]);
    }
    out->ok = true;
    return true;
}

bool HashBytes(const void* data, size_t len, Algo algo, std::wstring* hex) {
    if (!hex || (!data && len)) return false;
    hex->clear();

    HashCtx ctx;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&ctx.alg, AlgoId(algo), nullptr, 0);
    if (!NtOk(st)) return false;

    DWORD objLen = 0, got = 0;
    st = BCryptGetProperty(ctx.alg, BCRYPT_OBJECT_LENGTH,
                           reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0);
    if (!NtOk(st) || objLen == 0) return false;

    std::vector<unsigned char> obj(objLen);
    st = BCryptCreateHash(ctx.alg, &ctx.hash, obj.data(), objLen, nullptr, 0, 0);
    if (!NtOk(st)) return false;

    // len==0 时 BCryptHashData 收到空指针会报参数错；给一个非空的哑指针。
    static const unsigned char kDummy = 0;
    const unsigned char* p = len ? static_cast<const unsigned char*>(data) : &kDummy;
    if (!NtOk(BCryptHashData(ctx.hash, const_cast<PUCHAR>(p), static_cast<ULONG>(len), 0)))
        return false;

    unsigned char digest[64]{};
    DWORD digestLen = 0;
    st = BCryptGetProperty(ctx.alg, BCRYPT_HASH_LENGTH,
                           reinterpret_cast<PUCHAR>(&digestLen), sizeof(digestLen), &got, 0);
    if (!NtOk(st) || digestLen == 0 || digestLen > sizeof(digest)) return false;
    if (!NtOk(BCryptFinishHash(ctx.hash, digest, digestLen, 0))) return false;

    static const wchar_t* kHex = L"0123456789abcdef";
    hex->reserve(static_cast<size_t>(digestLen) * 2);
    for (DWORD i = 0; i < digestLen; ++i) {
        hex->push_back(kHex[digest[i] >> 4]);
        hex->push_back(kHex[digest[i] & 0xF]);
    }
    return true;
}

}  // namespace filehash
