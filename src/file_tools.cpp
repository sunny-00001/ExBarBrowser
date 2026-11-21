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

// 遍历 root 子树，把每个文件交给 sink(rel, full, bytes)。
// 返回 false = 预算用完提前结束（已交出的条目仍然有效）。
bool Walk(const std::wstring& root, const Options& opt, Clock::time_point deadline,
          const std::function<void(const std::wstring&, const std::wstring&,
                                   unsigned long long)>& sink,
          unsigned* denied) {
    unsigned den = 0;
    bool timedOut = false;

    std::vector<WalkItem> stack;
    stack.push_back({root, L""});

    while (!stack.empty()) {
        if (Clock::now() > deadline) {
            timedOut = true;
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
            if ((++inner & 0x3FF) == 0 && Clock::now() > deadline) {
                timedOut = true;
                break;
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

        if (timedOut) break;
    }

    if (denied) *denied = den;
    return !timedOut;
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
Content SameContent(const std::wstring& a, const std::wstring& b) {
    filehash::Result ha, hb;
    if (!filehash::HashFile(a, filehash::Algo::Sha256, &ha)) return Content::Unknown;
    if (!filehash::HashFile(b, filehash::Algo::Sha256, &hb)) return Content::Unknown;
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
    const auto deadline = t0 + std::chrono::milliseconds(opt.budgetMs);

    // 第一轮：只按大小分组。读内容很贵（几百 MB 的文件几秒钟），而大小不同的
    // 两份文件**一定**不是重复 —— 先用便宜的判据把候选压到极少数。
    std::unordered_map<unsigned long long, std::vector<DupFile>> bySize;
    unsigned denied = 0;
    const bool walked = Walk(
        out->root, opt, deadline,
        [&](const std::wstring&, const std::wstring& full, unsigned long long bytes) {
            ++out->scannedFiles;
            if (bytes < opt.minBytes) return;
            bySize[bytes].push_back({full, bytes});
        },
        &denied);
    out->deniedDirs = denied;

    // 大的先算：预算不够时，用户最想看到的是"这几个 GB 的视频重复了"。
    std::vector<std::pair<unsigned long long, std::vector<DupFile>*>> cand;
    for (auto& kv : bySize)
        if (kv.second.size() >= 2) cand.push_back({kv.first, &kv.second});
    std::sort(cand.begin(), cand.end(),
              [](const auto& x, const auto& y) { return x.first > y.first; });

    bool timedOut = !walked;
    for (auto& [size, files] : cand) {
        if (timedOut) break;

        std::unordered_map<std::wstring, std::vector<DupFile>> byHash;
        bool groupDone = true;
        for (const auto& f : *files) {
            if (Clock::now() > deadline) {
                timedOut = true;
                groupDone = false;
                break;
            }
            filehash::Result hr;
            if (!filehash::HashFile(f.path, filehash::Algo::Sha256, &hr)) {
                // 打不开/被独占的文件跳过，但**不**把它当成"不重复"的证据 ——
                // 它只是没参与比较，报告里的 hashedFiles 会少一个。
                continue;
            }
            ++out->hashedFiles;
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
    out->complete = !timedOut;
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
    const auto deadline = t0 + std::chrono::milliseconds(opt.walk.budgetMs);

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
    const bool walkedL = Walk(
        out->left, opt.walk, deadline,
        [&](const std::wstring& rel, const std::wstring& full, unsigned long long bytes) {
            Side s;
            s.rel = rel;
            s.full = full;
            s.bytes = bytes;
            lefts[str::Lower(rel)] = std::move(s);
            out->leftBytes += bytes;
        },
        &deniedL);

    bool timedOut = !walkedL;
    if (!timedOut) {
        const bool walkedR = Walk(
            out->right, opt.walk, deadline,
            [&](const std::wstring& rel, const std::wstring& full, unsigned long long bytes) {
                out->rightBytes += bytes;
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
                    const Content r = SameContent(it->second.full, full);
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
        timedOut = !walkedR;
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
    out->complete = !timedOut;
    out->elapsedMs = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return true;
}

// ---- 批量改名的"计划" ----

namespace {

// 拆成 主名 / 扩展名。注意用**最后一个点**：`a.tar.gz` → 主名 `a.tar`、扩展名 `.gz`。
// 用第一个点会把 `my.report.final.docx` 拆错，而这类名字在下载目录里到处都是。
void SplitName(const std::wstring& name, std::wstring* stem, std::wstring* ext) {
    const size_t dot = name.rfind(L'.');
    // 开头的点（隐藏文件风格 `.gitignore`）不算扩展名分隔
    if (dot == std::wstring::npos || dot == 0) {
        *stem = name;
        ext->clear();
        return;
    }
    *stem = name.substr(0, dot);
    *ext = name.substr(dot);
}

std::wstring PadNum(int n, size_t width) {
    std::wstring s = std::to_wstring(n);
    while (s.size() < width) s.insert(s.begin(), L'0');
    return s;
}

// Windows 文件名里不允许出现的字符。冒号单列出来是给"驱动器/备用流"用的，
// 但在这里同样不允许 —— 带冒号的名字在资源管理器里根本建不出来。
bool HasInvalidChar(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c < 0x20) return true;
        switch (c) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"': case L'<': case L'>': case L'|':
            return true;
        default:
            break;
        }
    }
    return false;
}

bool ExpandPattern(const std::wstring& pattern, const std::wstring& oldName, int index,
                   std::wstring* out, bool* usedAny, bool* usedExt) {
    if (usedExt) *usedExt = false;
    std::wstring stem, ext;
    SplitName(oldName, &stem, &ext);
    std::wstring r;
    r.reserve(pattern.size() + 16);
    *usedAny = false;

    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] != L'{') {
            r.push_back(pattern[i]);
            ++i;
            continue;
        }
        const size_t close = pattern.find(L'}', i + 1);
        if (close == std::wstring::npos) {
            // 没有闭合：当普通字符，不报错（用户可能就想要一个左花括号）
            r.push_back(pattern[i]);
            ++i;
            continue;
        }
        const std::wstring token = str::Lower(pattern.substr(i + 1, close - i - 1));
        bool replaced = true;
        if (token == L"name") r += stem;
        else if (token == L"ext") {
            r += ext;
            if (usedExt) *usedExt = true;
        }
        else if (token == L"old") {
            r += oldName;
            if (usedExt) *usedExt = true;   // {old} 已含扩展名，不能再追加一次
        }
        else if (token == L"n") r += PadNum(index, 1);
        else if (token == L"nn") r += PadNum(index, 2);
        else if (token == L"nnn") r += PadNum(index, 3);
        else replaced = false;   // 认不出的占位符原样保留

        if (replaced) {
            *usedAny = true;
            i = close + 1;
        } else {
            r.push_back(pattern[i]);   // 逐字符前进，最终会把整段原样输出
            ++i;
        }
    }
    *out = r;
    return true;
}

}  // namespace

bool PlanBatchRename(const std::vector<std::wstring>& oldNames,
                     const std::vector<std::wstring>& existing, const std::wstring& pattern,
                     std::vector<RenamePair>* plan, unsigned* skippedSame, std::wstring* err) {
    if (!plan) return false;
    plan->clear();
    if (skippedSame) *skippedSame = 0;

    if (pattern.empty()) {
        if (err) *err = L"模式为空";
        return false;
    }
    if (oldNames.empty()) {
        if (err) *err = L"没有要改名的项目";
        return false;
    }

    // 一次预扫描：有没有任何有效占位符。没有的话每个结果都相同，必然重名，
    // 直接说清楚（否则用户会收到一条"第 2 项重名"的误导性报错）。
    {
        bool dummy = false, dummyExt = false;
        std::wstring tmp;
        ExpandPattern(pattern, L"x.txt", 1, &tmp, &dummy, &dummyExt);
        if (!dummy) {
            if (err) *err = L"模式里没有占位符（可用 {name} {ext} {old} {n} {nn} {nnn}）";
            return false;
        }
    }

    std::vector<std::wstring> created;
    created.reserve(oldNames.size());

    for (size_t k = 0; k < oldNames.size(); ++k) {
        const std::wstring& oldName = oldNames[k];
        std::wstring newName;
        bool used = false, usedExt = false;
        ExpandPattern(pattern, oldName, static_cast<int>(k) + 1, &newName, &used, &usedExt);

        // 模式里没写 {ext} 就**自动保留原扩展名**。
        // 理由：批量改名最怕的是文件失去关联（双击打不开），而"改前半段"
        // （`{name}_{n}`）是最常见的意图。要改扩展名就显式写 {ext}，
        // 模式串里写字面量扩展名（`{name}_x.jpg`）会得到 `a_x.jpg.txt` ——
        // 这在预览里一眼可见，比"悄悄丢掉 .txt"安全得多。
        if (!usedExt) {
            std::wstring stem0, ext0;
            SplitName(oldName, &stem0, &ext0);
            newName += ext0;
        }

        if (newName.empty()) {
            if (err) *err = L"“" + oldName + L"”算出了空名字";
            return false;
        }
        if (HasInvalidChar(newName)) {
            if (err) *err = L"“" + newName + L"”含文件名不允许的字符";
            return false;
        }
        // 末尾的空格/点在资源管理器里会被静默去掉，导致"改名成功但名字不对"
        if (newName.back() == L' ' || newName.back() == L'.') {
            if (err) *err = L"“" + newName + L"”不能以空格或点结尾";
            return false;
        }
        if (newName.size() > 255) {
            if (err) *err = L"“" + newName + L"”超过 255 个字符";
            return false;
        }

        if (str::IEquals(newName, oldName)) {
            if (skippedSame) ++*skippedSame;   // 没变化的不算失败，也不动手
            continue;
        }

        // 批内重名（大小写不敏感：Windows 上 a.txt 与 A.TXT 是同一个文件）
        bool dupInBatch = false;
        for (const auto& c : created)
            if (str::IEquals(c, newName)) dupInBatch = true;
        if (dupInBatch) {
            if (err) *err = L"多于一项目标同名：“" + newName + L"”";
            return false;
        }

        // 撞到目录里现有的、且**不在本批**的文件
        bool clash = false;
        for (const auto& e : existing) {
            if (str::IEquals(e, oldName)) continue;   // 自己不算冲突
            if (str::IEquals(e, newName)) {
                clash = true;
                break;
            }
        }
        if (clash) {
            if (err) *err = L"目标已存在：“" + newName + L"”";
            return false;
        }

        created.push_back(newName);
        plan->push_back({oldName, newName});
    }

    if (plan->empty()) {
        if (err) *err = L"没有任何文件需要改名（模式算出的名字与原名相同）";
        return false;
    }
    return true;
}

}  // namespace filetools
