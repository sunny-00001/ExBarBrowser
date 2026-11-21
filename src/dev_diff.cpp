// 结构化 diff 引擎的实现（接口说明见 dev_diff.h）。
#include "dev_diff.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "dev_diff_parse.h"
#include "dev_proc.h"
#include "file_hash.h"
#include "str.h"
#include "text_file.h"

namespace devdiff {
namespace {

// ---------------------------------------------------------------- git 调用

// 跑一条 git 命令，把 stdout 收下来。
//
// 【为什么统一在这里加 -c core.quotepath=false】
// 默认 git 会把非 ASCII 路径转义成 "a\346\226\207.txt" 这种八进制串 ——
// 拿它当路径去读文件必然失败，而这个失败表现为"这个文件没有改动"，
// 极难归因。core.quotepath=false 让它输出原始路径。
//
// 【为什么输出上限必须知道】devproc 的输出上限是 256 KB，超了会**静默截断**。
// 一份被截断的 patch 看起来和完整的 patch 一模一样 —— 那是最坏的一种假绿：
// 审阅者以为看全了。所以这里把 overflow 报出来，交给上层如实标记。
bool GitIn(const std::wstring& root, const std::vector<std::wstring>& args, int* code,
           std::wstring* out, bool* overflow) {
    std::vector<std::wstring> full;
    full.push_back(L"-c");
    full.push_back(L"core.quotepath=false");
    for (const std::wstring& a : args) full.push_back(a);
    std::wstring err;
    // **必须把 overflow 透传下去**：不传的话截断信息在这里就丢了，
    // 上层永远看不到"内容不完整" —— 截断后的 patch 和完整的 patch 一模一样。
    return devproc::RunCapture(L"git", full, root, code, out, &err, overflow);
}

// ---------------------------------------------------------------- 未跟踪文件

// 把一段文本切成行（去掉行尾 \r）。
//
// 【为什么去掉 \r】工作区可能是 CRLF、HEAD 里是 LF（core.autocrlf 或
// 编辑器设置都会造成）。不归一化的话，同一个文件会"每一行都不同" ——
// 那不是改动，是噪声，而噪声会把真改动淹没。
std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        std::wstring l = text.substr(start, end - start);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(std::move(l));
        start = end + 1;
    }
    // 末尾多一个空行（文件以换行结尾）时，那不是一行内容。
    if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == L'\n') {
        lines.pop_back();
    }
    return lines;
}

// 是不是二进制（含 NUL 字节）。
bool LooksBinary(const std::string& bytes) {
    return bytes.find('\0') != std::string::npos;
}

// 绝对路径 = root + 相对路径。
std::wstring AbsOf(const std::wstring& root, const std::wstring& rel) {
    std::wstring r = rel;
    for (wchar_t& c : r)
        if (c == L'/') c = L'\\';
    if (root.empty()) return r;
    if (root.back() == L'\\') return root + r;
    return root + L"\\" + r;
}

// 把仓库相对路径归一成 git 的写法：**斜杠分隔**。
//
// git 的路径一律用 '/'（即使在 Windows 上），而调用方给的可能是反斜杠
// （Windows 习惯）。不归一化就会"过滤不中" —— 而"过滤不中"的表现是
// "这个文件没有改动"，看着像真的。
std::wstring NormalizeRel(const std::wstring& p) {
    std::wstring r = p;
    for (wchar_t& c : r)
        if (c == L'\\') c = L'/';
    return r;
}

// 未跟踪文件 → 合成一个"全新文件"的 hunk。
//
// 【为什么必须自己合成】git 的 diff 只比**已跟踪**的路径：未跟踪的新文件
// 在 `git diff` 里一个字都没有。而自我迭代场景里 agent 写出来的**新文件**
// 恰恰是最该被审的那部分 —— 只给 patch 文本的实现等于"新文件写了什么，
// 审阅者一个字都看不到"，却还报 added=0，看起来像"这个文件没改动"。
Hunk MakeNewFileHunk(const std::wstring& path, const std::vector<std::wstring>& lines) {
    Hunk h;
    h.oldStart = 0;
    h.oldLines = 0;
    h.newStart = 1;
    h.newLines = static_cast<int>(lines.size());
    h.header = L"@@ -0,0 +1," + std::to_wstring(lines.size()) + L" @@";
    for (const std::wstring& l : lines) {
        h.text += L"+";
        h.text += l;
        h.text += L"\n";
    }
    (void)path;
    return h;
}

}  // namespace

Report Compute(const Options& opt) {
    Report rep;
    const std::wstring root = opt.root.empty() ? tf::ExeDir() : opt.root;

    int code = 0;
    std::wstring out;
    bool overflow = false;
    if (!GitIn(root, {L"rev-parse", L"--short", L"HEAD"}, &code, &out, &overflow) || code != 0) {
        rep.error = L"git rev-parse 失败（仓库不可用？）";
        return rep;
    }
    rep.head = str::Trim(out);

    // ---- 1. 改动清单（权威来源：工作树相对 HEAD）----
    //
    // 清单、行数、内容**全部出自 diff HEAD 这一条线** ——
    // 混用 git status（索引口径）会让"清单说重命名、patch 里是删+增"对不上。
    std::wstring nameStatusText;
    bool nameStatusOverflow = false;
    if (!GitIn(root, {L"diff", L"HEAD", L"--name-status", L"-z"}, &code, &nameStatusText,
               &nameStatusOverflow) ||
        code != 0) {
        rep.error = L"git diff --name-status 失败";
        return rep;
    }
    std::vector<ChangeEntry> entries = ParseNameStatusZ(nameStatusText);

    // 未跟踪文件：git diff 的所有变体都看不见它们，必须单独取。
    std::wstring untrackedText;
    bool untrackedOverflow = false;
    GitIn(root, {L"ls-files", L"--others", L"--exclude-standard", L"-z"}, &code,
         &untrackedText, &untrackedOverflow);
    for (const std::wstring& p : ParseUntrackedZ(untrackedText)) {
        ChangeEntry e;
        e.status = L"untracked";
        e.path = p;
        e.tracked = false;
        entries.push_back(std::move(e));
    }
    rep.totalFiles = entries.size();

    // ---- 2. 行数统计与 patch（与清单同源）----
    std::wstring numText;
    GitIn(root, {L"diff", L"HEAD", L"--numstat", L"-z"}, &code, &numText, &overflow);
    const std::map<std::wstring, Numstat> numstat = ParseNumstatZ(numText);

    std::wstring patchText;
    bool patchOverflow = false;
    GitIn(root, {L"diff", L"HEAD", L"--no-ext-diff", L"--no-color", L"--src-prefix=a/",
                 L"--dst-prefix=b/"},
          &code, &patchText, &patchOverflow);
    std::map<std::wstring, Section> sections;
    for (const Section& s : ParsePatch(patchText)) sections[s.path] = s;

    // ---- 3. 逐文件装配 ----
    const std::wstring want = NormalizeRel(opt.onlyPath);
    size_t totalBytes = 0;
    size_t shown = 0;
    for (const ChangeEntry& e : entries) {
        // 过滤：**新旧路径都算**。重命名时用旧路径问也能问到 ——
        // 只认新路径的话，"审一下改名之前那个文件"会返回"没有改动"。
        if (!want.empty() && NormalizeRel(e.path) != want &&
            (e.origPath.empty() || NormalizeRel(e.origPath) != want)) {
            continue;
        }
        if (shown >= opt.maxFiles) {
            rep.truncated = true;
            rep.truncatedReason = L"改动文件数超过上限，只展开了前 " +
                                  std::to_wstring(opt.maxFiles) +
                                  L" 个；其余用 path 参数逐个看";
            break;
        }
        ++shown;

        FileDiff fd;
        fd.path = e.path;
        fd.status = e.status;
        fd.origPath = e.origPath;
        fd.tracked = e.tracked;

        // 字节数与摘要：只有工作树里**还在**的文件才有。
        // 删除掉的文件取不到（sha256 空串 = "没有"，不是"内容是空的"）。
        const std::wstring abs = AbsOf(root, e.path);
        if (tf::Exists(abs) && !tf::IsDirectory(abs)) {
            filehash::Result hr;
            if (filehash::HashFile(abs, filehash::Algo::Sha256, &hr) && hr.ok) {
                fd.sha256 = hr.hex;
                fd.bytes = hr.bytes;
            }
        }
        fd.mtime = tf::FileMtimeUnix(abs);

        auto it = numstat.find(e.path);
        if (it != numstat.end()) {
            fd.added = it->second.added;
            fd.deleted = it->second.deleted;
            if (it->second.binary) {
                fd.truncated = true;
                fd.reason = L"二进制文件（没有行级差异，只给摘要）";
            }
        }

        // 逐行内容：优先用 git 的 patch 段落；未跟踪文件自己合成。
        auto sit = sections.find(e.path);
        if (sit != sections.end() && !sit->second.binary) {
            for (const Hunk& h : sit->second.hunks) {
                if (totalBytes + h.text.size() > opt.maxTotalBytes ||
                    fd.hunks.size() > 0 && h.text.size() > opt.maxBytesPerFile) {
                    fd.truncated = true;
                    if (fd.reason.empty())
                        fd.reason = L"diff 文本超过预算，只给了前几段";
                    break;
                }
                totalBytes += h.text.size();
                fd.hunks.push_back(h);
            }
        } else if (!fd.tracked) {
            // 未跟踪文件：合成"全新文件"的一段。
            std::string bytes;
            if (tf::ReadAllBytes(abs, &bytes)) {
                if (LooksBinary(bytes)) {
                    fd.truncated = true;
                    fd.reason = L"二进制文件（没有行级差异，只给摘要）";
                } else {
                    std::wstring text;
                    if (!str::FromUtf8Strict(bytes, &text)) {
                        fd.truncated = true;
                        fd.reason = L"不是 UTF-8 文本（没有行级差异，只给摘要）";
                    } else {
                        const std::vector<std::wstring> lines = SplitLines(text);
                        Hunk h = MakeNewFileHunk(e.path, lines);
                        if (h.text.size() > opt.maxBytesPerFile) {
                            fd.truncated = true;
                            fd.reason = L"文件太大，只给了行数统计";
                            fd.added = static_cast<int>(lines.size());
                        } else {
                            totalBytes += h.text.size();
                            fd.hunks.push_back(std::move(h));
                            fd.added = static_cast<int>(lines.size());
                            fd.deleted = 0;
                        }
                    }
                }
            } else {
                fd.truncated = true;
                fd.reason = L"读不出文件内容（可能已被删除或被独占）";
            }
        } else if (sit == sections.end()) {
            // git 没给这一项的段落。**要区分两种情况**：
            //   · 没有行数变化（added=0 且 deleted=0）→ 纯重命名 / 只改了文件模式。
            //     这些**本来就没有行级差异**，不是"我们没取到"——
            //     标成 truncated 会让审阅者以为缺了内容，白白去查。
            //   · 有行数变化却没段落 → 真的取不到，如实说。
            if (fd.added == 0 && fd.deleted == 0) {
                fd.reason = L"没有行级差异（纯重命名或只改了文件模式）";
            } else {
                fd.truncated = true;
                fd.reason = L"git 没有给出这一项的 diff 段落（内容可能不完整）";
            }
        }

        rep.files.push_back(std::move(fd));
    }

    if (patchOverflow) {
        rep.truncated = true;
        if (rep.truncatedReason.empty())
            rep.truncatedReason = L"git diff 的输出超过 256 KB 上限，被截断了（内容不完整）";
    }
    if (nameStatusOverflow) {
        rep.truncated = true;
        if (rep.truncatedReason.empty())
            rep.truncatedReason = L"git diff --name-status 的输出超过 256 KB 上限，被截断了（改动清单不完整）";
    }
    if (untrackedOverflow) {
        rep.truncated = true;
        if (rep.truncatedReason.empty())
            rep.truncatedReason = L"git ls-files 的输出超过 256 KB 上限，被截断了（未跟踪文件清单不完整）";
    }
    if (opt.onlyPath.empty() && rep.files.empty() && rep.totalFiles == 0) {
        rep.note = L"工作树没有改动";
    } else if (!opt.onlyPath.empty() && rep.files.empty()) {
        rep.note = L"这个路径不在改动清单里（没有未提交的改动）";
    }
    rep.ok = true;
    return rep;
}

}  // namespace devdiff
