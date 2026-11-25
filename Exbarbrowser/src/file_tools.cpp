// 文件集工具实现（说明见 file_tools.h）。
#include "file_tools.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_map>

#include "file_hash.h"
#include "str.h"
#include "text_file.h"  // tf::ToExtended：>260 字符路径需要扩展长度前缀

namespace filetools {
namespace {

using Clock = std::chrono::steady_clock;

struct WalkItem {
    std::wstring full;
    std::wstring rel;
};

// 提前结束的原因。三种必须分开：调用方要据此决定"重试"还是"放弃"。
enum class Abort { None, Timeout, Cancelled };

// 遍历 root 子树，把每个文件交给 sink(rel, full, bytes)。
// 返回 None = 走完了；其余 = 提前结束（已交出的条目仍然有效）。
Abort Walk(const std::wstring& root, const Options& opt, Clock::time_point deadline,
           const std::function<void(const std::wstring&, const std::wstring&,
                                   unsigned long long)>& sink,
           unsigned* denied) {
    unsigned den = 0;
    Abort abort = Abort::None;

    std::vector<WalkItem> stack;
    stack.push_back({root, L""});

    while (!stack.empty()) {
        if (Clock::now() > deadline) {
            abort = Abort::Timeout;
            break;
        }
        // 取消：置位即停。
        if (opt.ctl && opt.ctl->Cancelled()) {
            abort = Abort::Cancelled;
            break;
        }
        const WalkItem cur = std::move(stack.back());
        stack.pop_back();

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(tf::ToExtended(cur.full + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            // 枚举不了（权限/被占用/路径过长）：计数上报，不静默跳过。
            ++den;
            continue;
        }

        unsigned inner = 0;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            // 单个目录里几十万文件时，只在目录入口查预算等于没查（同 disk_scan 的坑）。
            if ((++inner & 0x3FF) == 0) {
                if (Clock::now() > deadline) {
                    abort = Abort::Timeout;
                    break;
                }
                if (opt.ctl && opt.ctl->Cancelled()) {
                    abort = Abort::Cancelled;
                    break;
                }
            }

            const std::wstring name = fd.cFileName;
            const std::wstring rel = cur.rel.empty() ? name : cur.rel + L"\\" + name;
            const std::wstring full = cur.full + (cur.full.empty() ? L"" : L"\\") + name;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    !opt.followReparsePoints)
                    continue;
                stack.push_back({full, rel});
            } else {
                const unsigned long long sz =
                    (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                sink(rel, full, sz);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if (abort != Abort::None) break;
    }

    if (denied) *denied = den;
    return abort;
}

// 目标必须是存在的目录。dupfinder / compare 的入口都先过这一关，
// 错误信息区分"不存在"与"是文件"，否则用户拿到的提示帮不上忙。
bool RequireDir(const std::wstring& path, const wchar_t* label, std::wstring* err) {
    const DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = std::wstring(label ? label : L"") + L"路径不存在或无法访问";
        return false;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (err) *err = std::wstring(label ? label : L"") + L"不是目录";
        return false;
    }
    return true;
}

// 去掉末尾的分隔符（"C:\" 要保留根的反斜杠，否则拼出来是 C: 的相对路径）。
std::wstring TrimSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (p.size() == 2 && p[1] == L':') return p;
    return p;
}

// 内容比较的三态：相同 / 不同 / 读不了。第三态不能并入任何一边 ——
// 把"读不了"说成"相同"，用户会漏掉真正的差异；说成"不同"，用户会去核对一份好文件。
enum class Content { Same, Differ, Unknown };
Content SameContent(const std::wstring& a, const std::wstring& b,
                    const std::atomic<bool>* cancel) {
    filehash::Result ha, hb;
    if (!filehash::HashFile(a, filehash::Algo::Sha256, &ha, cancel)) return Content::Unknown;
    if (!filehash::HashFile(b, filehash::Algo::Sha256, &hb, cancel)) return Content::Unknown;
    return ha.hex == hb.hex ? Content::Same : Content::Differ;
}

}  // namespace

bool FindDuplicates(const std::wstring& root, const Options& opt, DupResult* out,
                    std::wstring* err) {
    if (!out) return false;
    *out = DupResult{};
    out->root = TrimSlash(root);
    if (!RequireDir(out->root, L"", err)) return false;

    const auto t0 = Clock::now();
    // 不限时：取"永远到不了"的时刻（理由见 scan_ctl.h）。
    const auto deadline = opt.noTimeLimit ? Clock::time_point::max()
                                           : t0 + std::chrono::milliseconds(opt.budgetMs);

    // 第一轮：只按大小分组。读内容很贵（几百 MB 的文件几秒钟），而大小不同的
    // 两份文件**一定**不是重复 —— 先用便宜的判据把候选压到极少数。
    std::unordered_map<unsigned long long, std::vector<DupFile>> bySize;
    unsigned denied = 0;
    const Abort walkAbort = Walk(
        out->root, opt, deadline,
        [&](const std::wstring&, const std::wstring& full, unsigned long long bytes) {
            ++out->scannedFiles;
            if (bytes < opt.minBytes) return;
            bySize[bytes].push_back({full, bytes});
            // 进度：已扫描文件数 / 总量未知（遍历前不知道有多少个）。
            if (opt.ctl) opt.ctl->Report(L"正在扫描文件", out->scannedFiles, 0);
        },
        &denied);
    out->deniedDirs = denied;

    // 大的先算：预算不够时，用户最想看到的是"这几个 GB 的视频重复了"。
    std::vector<std::pair<unsigned long long, std::vector<DupFile>*>> cand;
    for (auto& kv : bySize)
        if (kv.second.size() >= 2) cand.push_back({kv.first, &kv.second});
    std::sort(cand.begin(), cand.end(),
              [](const auto& x, const auto& y) { return x.first > y.first; });

    bool timedOut = walkAbort == Abort::Timeout;
    bool cancelled = walkAbort == Abort::Cancelled;
    unsigned long long hashTotal = 0;
    for (auto& [size, files] : cand) hashTotal += files->size();
    unsigned long long hashDone = 0;
    for (auto& [size, files] : cand) {
        if (timedOut || cancelled) break;

        std::unordered_map<std::wstring, std::vector<DupFile>> byHash;
        bool groupDone = true;
        for (const auto& f : *files) {
            if (Clock::now() > deadline) {
                timedOut = true;
                groupDone = false;
                break;
            }
            if (opt.ctl && opt.ctl->Cancelled()) {
                cancelled = true;
                groupDone = false;
                break;
            }
            filehash::Result hr;
            if (!filehash::HashFile(f.path, filehash::Algo::Sha256, &hr, opt.ctl ? opt.ctl->cancel : nullptr)) {
                // 打不开/被独占的文件跳过，但**不**把它当成"不重复"的证据 ——
                // 它只是没参与比较，报告里的 hashedFiles 会少一个。
                continue;
            }
            ++out->hashedFiles;
            ++hashDone;
            if (opt.ctl) opt.ctl->Report(L"正在计算摘要", hashDone, hashTotal);
            byHash[hr.hex].push_back(f);
        }
        // 组内只认完整哈希过的组：拿着半组结果宣称"只有两份"是错的。
        if (!groupDone) break;

        for (auto& kv : byHash) {
            if (kv.second.size() < 2) continue;
            DupGroup g;
            g.bytes = size;
            g.hash = kv.first;
            g.files = std::move(kv.second);
            std::sort(g.files.begin(), g.files.end(),
                      [](const DupFile& x, const DupFile& y) { return x.path < y.path; });
            out->wastedBytes += size * (g.files.size() - 1);
            out->groups.push_back(std::move(g));
        }
    }

    std::sort(out->groups.begin(), out->groups.end(), [](const DupGroup& a, const DupGroup& b) {
        const unsigned long long wa = a.bytes * (a.files.size() - 1);
        const unsigned long long wb = b.bytes * (b.files.size() - 1);
        if (wa != wb) return wa > wb;
        return a.bytes > b.bytes;
    });

    out->timedOut = timedOut;
    out->cancelled = cancelled;
    out->complete = !timedOut && !cancelled;
    out->elapsedMs = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return true;
}

bool CompareFolders(const std::wstring& left, const std::wstring& right,
                    const CompareOptions& opt, CompareResult* out, std::wstring* err) {
    if (!out) return false;
    *out = CompareResult{};
    out->left = TrimSlash(left);
    out->right = TrimSlash(right);
    if (!RequireDir(out->left, L"左侧", err)) return false;
    if (!RequireDir(out->right, L"右侧", err)) return false;

    const auto t0 = Clock::now();
    // 不限时：取"永远到不了"的时刻（理由见 scan_ctl.h）。
    const auto deadline = opt.walk.noTimeLimit
                              ? Clock::time_point::max()
                              : t0 + std::chrono::milliseconds(opt.walk.budgetMs);

    // key 用小写相对路径：Windows 文件系统大小写不敏感，"A.TXT" 与 "a.txt"
    // 是同一份文件，按原样做键会把它们当成两条 OnlyLeft/OnlyRight。
    struct Side {
        std::wstring rel;      // 展示用（保留原始大小写）
        std::wstring full;
        unsigned long long bytes = 0;
        bool seen = false;
    };
    std::unordered_map<std::wstring, Side> lefts;

    unsigned deniedL = 0, deniedR = 0;
    const Abort walkL = Walk(
        out->left, opt.walk, deadline,
        [&](const std::wstring& rel, const std::wstring& full, unsigned long long bytes) {
            Side s;
            s.rel = rel;
            s.full = full;
            s.bytes = bytes;
            lefts[str::Lower(rel)] = std::move(s);
            out->leftBytes += bytes;
            if (opt.walk.ctl) opt.walk.ctl->Report(L"正在扫描左侧", out->leftBytes, 0);
        },
        &deniedL);

    bool timedOut = walkL == Abort::Timeout;
    bool cancelled = walkL == Abort::Cancelled;
    if (!timedOut && !cancelled) {
        const Abort walkR = Walk(
            out->right, opt.walk, deadline,
            [&](const std::wstring& rel, const std::wstring& full, unsigned long long bytes) {
                out->rightBytes += bytes;
                if (opt.walk.ctl) opt.walk.ctl->Report(L"正在扫描右侧", out->rightBytes, 0);
                auto it = lefts.find(str::Lower(rel));
                if (it == lefts.end()) {
                    CompareEntry e;
                    e.rel = rel;
                    e.rightBytes = bytes;
                    e.state = Diff::OnlyRight;
                    ++out->onlyRightCount;
                    if (out->entries.size() < opt.maxEntries) out->entries.push_back(std::move(e));
                    else out->truncated = true;
                    return;
                }
                it->second.seen = true;
                CompareEntry e;
                e.rel = it->second.rel;
                e.leftBytes = it->second.bytes;
                e.rightBytes = bytes;
                if (it->second.bytes != bytes) {
                    e.state = Diff::Differ;
                    ++out->differCount;
                } else if (opt.byContent && it->second.bytes > 0) {
                    // 同大小不代表同一份（很多零填充文件、同名空模板）。
                    const Content r = SameContent(it->second.full, full,
                                                 opt.walk.ctl ? opt.walk.ctl->cancel : nullptr);
                    if (r == Content::Same) {
                        e.state = Diff::Same;
                        ++out->sameCount;
                    } else {
                        e.state = Diff::Differ;
                        ++out->differCount;
                    }
                } else {
                    e.state = Diff::Same;
                    ++out->sameCount;
                }
                if (e.state != Diff::Same) {
                    if (out->entries.size() < opt.maxEntries) out->entries.push_back(std::move(e));
                    else out->truncated = true;
                }
            },
            &deniedR);
        timedOut = walkR == Abort::Timeout;
        cancelled = walkR == Abort::Cancelled;
    }

    // 左侧独有的：seen 仍是 false 的那些。
    for (auto& kv : lefts) {
        if (kv.second.seen) continue;
        CompareEntry e;
        e.rel = kv.second.rel;
        e.leftBytes = kv.second.bytes;
        e.state = Diff::OnlyLeft;
        ++out->onlyLeftCount;
        if (out->entries.size() < opt.maxEntries) out->entries.push_back(std::move(e));
        else out->truncated = true;
    }

    std::sort(out->entries.begin(), out->entries.end(),
              [](const CompareEntry& a, const CompareEntry& b) { return a.rel < b.rel; });

    out->deniedLeft = deniedL;
    out->deniedRight = deniedR;
    out->timedOut = timedOut;
    out->cancelled = cancelled;
    out->complete = !timedOut && !cancelled;
    out->elapsedMs = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return true;
}

}  // namespace filetools
