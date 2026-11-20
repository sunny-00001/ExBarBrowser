// 目录体积统计实现（说明见 disk_scan.h）。
#include "disk_scan.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

#include "common.h"
#include "text_file.h"   // tf::ToExtended：>260 字符路径需要扩展长度前缀

namespace diskscan {
namespace {

using Clock = std::chrono::steady_clock;

// 递归统计一个目录的总字节数。
//
// 为什么用显式栈而不是递归函数：深层目录（几千层）会把调用栈打穿。
// 而且显式栈更容易在循环里检查 deadline。
struct Frame {
    std::wstring dir;
    unsigned long long bytes = 0;
    unsigned files = 0;
    unsigned dirs = 0;
    bool complete = true;
};

bool IsReparse(const WIN32_FIND_DATAW& fd) {
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// 统计 dir 子树。返回 false 表示因超时/出错没算完（bytes 仍是已算到的部分）。
bool SumSubtree(const std::wstring& dir, const Options& opt, Clock::time_point deadline,
                unsigned long long* bytes, unsigned* files, unsigned* dirs,
                unsigned* deniedOut = nullptr) {
    *bytes = 0;
    *files = 0;
    *dirs = 0;
    unsigned denied = 0;

    std::vector<std::wstring> stack;
    stack.push_back(dir);

    while (!stack.empty()) {
        // 每进入一个目录检查一次时间：足够细，开销可忽略。
        if (Clock::now() > deadline) {
            if (deniedOut) *deniedOut = denied;
            return false;
        }

        const std::wstring cur = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW fd{};
        // ToExtended：>260 字符的目录以前 FindFirstFileW 直接失败，被下面那行
        // 当成"权限不足"跳过 —— 于是它的体积计成 0，而 complete 仍是 true。
        // 用户看到的是一个"空"的大目录，与"真的就这么大"无法区分。
        const std::wstring pattern = tf::ToExtended(cur) + L"\\*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            // 真的枚举不了（权限/被占用）：计数并上报，不要静默跳过。
            ++denied;
            continue;
        }
        ++*dirs;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir) {
                // 不跟随重解析点：进一个 junction 可能绕回自己，无限递归。
                if (IsReparse(fd) && !opt.followReparsePoints) continue;
                stack.push_back(cur + L"\\" + fd.cFileName);
            } else {
                const unsigned long long sz =
                    (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                *bytes += sz;
                ++*files;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (deniedOut) *deniedOut = denied;
    return true;
}

}  // namespace

bool Scan(const std::wstring& path, const Options& opt, Result* out, std::wstring* err) {
    if (!out) return false;
    *out = Result{};
    out->path = path;

    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = L"路径不存在或无法访问";
        return false;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (err) *err = L"不是目录";
        return false;
    }

    const auto t0 = Clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(opt.budgetMs);

    // 第一层：列直接子项。子项各自才是"一个候选条目"。
    std::vector<Entry> found;
    {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = tf::ToExtended(path) + L"\\*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            if (err) *err = L"无法枚举目录内容";
            return false;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            Entry e;
            e.name = fd.cFileName;
            e.path = path + L"\\" + fd.cFileName;
            e.isFolder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            found.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 第二层：逐个算大小。
    //
    // 为什么逐个而不是一次遍历全树：这样"时间用完了"也能给出
    // 前几项的真实数值，而不是一个都没算完的空结果。du 也是这个思路
    // （先给能给的，最后说明是否完整）。
    for (auto& e : found) {
        if (Clock::now() > deadline) {
            out->timedOut = true;
            e.complete = false;
            continue;   // 其余条目的 bytes 保持 0 + complete=false
        }

        if (e.isFolder) {
            unsigned long long b = 0;
            unsigned f = 0, d = 0;
            unsigned denied = 0;
            const bool ok = SumSubtree(e.path, opt, deadline, &b, &f, &d, &denied);
            out->deniedDirs += denied;
            e.bytes = b;
            e.items = f + d;
            e.complete = ok;
            out->scannedFiles += f;
            out->scannedDirs += d;
            if (!ok) out->timedOut = true;
        } else {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(tf::ToExtended(e.path).c_str(), GetFileExInfoStandard,
                                     &fad)) {
                e.bytes = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
                          fad.nFileSizeLow;
                out->scannedFiles += 1;
            } else {
                e.complete = false;
            }
        }
    }

    // 按大小降序；同样大小按名字排，保证结果稳定（否则每次顺序不同，测试没法断言）
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        return a.name < b.name;
    });

    // 总量要含全部子项（截断前的），否则"总量 < 各子项之和"会让人困惑
    for (const auto& e : found) out->totalBytes += e.bytes;

    if (opt.limit > 0 && static_cast<int>(found.size()) > opt.limit) {
        found.resize(static_cast<size_t>(opt.limit));
        out->truncated = true;
    }
    out->entries = std::move(found);

    out->elapsedMs = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return true;
}

}  // namespace diskscan
