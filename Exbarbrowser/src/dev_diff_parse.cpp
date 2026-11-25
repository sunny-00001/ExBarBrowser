// git 输出的解析层（结构化 diff 引擎的实现见 src/dev_diff.cpp）。
//
// 【为什么拆出来】
//   "怎么解析 git 的输出"和"怎么把改动装配成报告"是两件事。
//   放一起时这个文件会涨到 650 行 —— 而**解析层是纯函数**：给定一段输出，
//   结果只取决于那段输出。所以它值得单独成文件：
//     · 能**脱离 git 单测**（喂一段 -z 文本，看解析出来的路径对不对）；
//     · 解析口径只有一份，改一处就够（分叉的代价是"有的路径看得见、
//       有的看不见"，且没人说得清为什么）。
//
// 【这一层踩过的坑（都在下面的注释里）】
//   · 重命名的字段顺序：`git status --porcelain -z` 是**新路径在前**，
//     而 `--name-status -z` / `--numstat -z` 是**旧路径在前** —— 方向相反。
//   · 普通（非 -z）输出会给带特殊字符的路径**加引号**，照 substr 切会得到
//     一个磁盘上不存在的路径，于是这个文件的改动被静默丢掉。
//   · 纯重命名时 git **不发** `---`/`+++` 行（只有 diff --git + rename from/to），
//     只认 ---/+++ 的话这一段会被整个丢掉。
#include "dev_diff_parse.h"

#include <map>
#include <string>
#include <vector>

namespace devdiff {
namespace {

std::vector<std::wstring> SplitNul(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        const size_t z = text.find(L'\0', start);
        if (z == std::wstring::npos) break;
        if (z > start) out.push_back(text.substr(start, z - start));
        start = z + 1;
    }
    if (start < text.size()) out.push_back(text.substr(start));
    return out;
}

// 解析 `git diff HEAD --name-status -z`。
//
// 格式：状态串\0 路径\0            —— 普通（M / A / D / T）
//       状态串\0 旧路径\0 新路径\0 —— 重命名/复制（R100 / C75）
//
// 【为什么以它为唯一权威清单，而不是 git status】
// 两个命令对"重命名"的**判定口径不一样**：
//   · `git status` 比的是**索引 vs HEAD**（用的是 git mv 记下的那条重命名）；
//   · `git diff HEAD` 比的是**工作树 vs HEAD**（按内容相似度**重新判定**）。
// 实测：`git mv b.txt d.txt` 之后又改了 d.txt 的内容，status 仍报 "R"，
// 而 diff HEAD 报的是 "D b.txt + A d.txt"（相似度掉到阈值以下）。
// 混用两个视图，就会出现"清单说 d.txt 是重命名、patch 段却对不上"的错位 ——
// 而错位的那一边会被静默丢掉。
//
// 所以这里只用 diff HEAD 这一条线：清单（name-status）、行数（numstat）、
// 内容（patch）都出自同一次比较，重命名判定必然一致。
//
// 【方向相反的坑】`git status --porcelain -z` 的重命名是**新路径在前、旧路径在后**，
// 而 `--name-status -z` 与 `--numstat -z` 都是**旧路径在前、新路径在后**。
// 两边方向相反，记反了就会取到另一个路径 —— 而旧路径在磁盘上已经没了。
bool ParseHunkHeader(const std::wstring& line, Hunk* h) {
    if (line.size() < 4 || line.compare(0, 3, L"@@ ") != 0) return false;
    size_t p = 3;
    // "-oldStart[,oldCount]"
    if (p >= line.size() || line[p] != L'-') return false;
    ++p;
    const size_t oldStart = _wtoi(line.c_str() + p);
    while (p < line.size() && line[p] >= L'0' && line[p] <= L'9') ++p;
    int oldLines = 1;
    if (p < line.size() && line[p] == L',') {
        ++p;
        oldLines = _wtoi(line.c_str() + p);
        while (p < line.size() && line[p] >= L'0' && line[p] <= L'9') ++p;
    }
    // 两段之间是**空格**（"@@ -1,3 +1,4 @@"）—— 忘了跳过就永远走不到 '+'，
    // 于是整份 patch 的 hunk 全被丢掉，报告变成"这个文件没有差异"。
    while (p < line.size() && line[p] == L' ') ++p;
    // "+newStart[,newCount]"
    if (p >= line.size() || line[p] != L'+') return false;
    ++p;
    const size_t newStart = _wtoi(line.c_str() + p);
    while (p < line.size() && line[p] >= L'0' && line[p] <= L'9') ++p;
    int newLines = 1;
    if (p < line.size() && line[p] == L',') {
        ++p;
        newLines = _wtoi(line.c_str() + p);
        while (p < line.size() && line[p] >= L'0' && line[p] <= L'9') ++p;
    }
    h->oldStart = oldStart;
    h->oldLines = oldLines;
    h->newStart = newStart;
    h->newLines = newLines;
    return true;
}

// 去掉 unified diff 的路径前缀（"a/" 或 "b/"）。
//
// 【为什么前缀要写死】git 的前缀受 diff.noprefix / diff.mnemonicPrefix 影响，
// 配置一改，"a/foo" 就变成 "i/foo" 或 "foo" —— 按固定长度切就会切错。
// 所以调用 git 时显式传 --src-prefix=a/ --dst-prefix=b/，这里再按同样口径剥。
std::wstring StripPrefix(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L'/') return p.substr(2);
    return p;
}

// 从 "diff --git a/X b/Y" 这一行里取路径（**兜底**用）。
//
// 【为什么只当兜底】带特殊字符的路径在这一行里是**加引号**的
// （"a/my file.txt" "b/my file.txt"），按固定位置切出来的结果不可靠。
// 好在它只在"既没有 ---/+++ 也没有 rename to"时才用得上 ——
// 那种情况是纯模式变化，路径就是原路径，取错的风险很低。
//
// 取的是 b/ 侧（新路径）：重命名时它才是磁盘上真实存在的那个路径。
std::wstring HeaderPathOf(const std::wstring& line) {
    size_t best = std::wstring::npos;
    for (size_t i = line.size(); i > 0; --i) {
        if (line.compare(i - 1, 3, L" b/") == 0) {
            best = i - 1;
            break;
        }
    }
    if (best == std::wstring::npos) return L"";
    std::wstring p = line.substr(best + 3);
    if (!p.empty() && p.back() == L'"') p.pop_back();
    return StripPrefix(p);
}


}  // namespace

std::vector<ChangeEntry> ParseNameStatusZ(const std::wstring& text) {
    std::vector<ChangeEntry> out;
    const std::vector<std::wstring> f = SplitNul(text);
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i].empty()) continue;
        const wchar_t letter = f[i][0];
        ChangeEntry e;
        switch (letter) {
            case L'A': e.status = L"added"; break;
            case L'D': e.status = L"deleted"; break;
            case L'M': e.status = L"modified"; break;
            case L'T': e.status = L"typechange"; break;
            case L'R': e.status = L"renamed"; break;
            case L'C': e.status = L"copied"; break;
            default:  e.status = L"modified"; break;  // 未知字母按"改了"处理，不静默丢弃
        }
        const bool twoPaths = (letter == L'R' || letter == L'C');
        if (twoPaths && i + 2 < f.size()) {
            e.origPath = f[i + 1];   // 旧路径在前
            e.path = f[i + 2];       // 新路径在后
            i += 2;
        } else if (i + 1 < f.size()) {
            e.path = f[i + 1];
            i += 1;
        } else {
            continue;               // 字段不完整：跳过这一条，不猜
        }
        if (!e.path.empty()) out.push_back(std::move(e));
    }
    return out;
}

// 解析 `git ls-files --others --exclude-standard -z` → 未跟踪文件。
//
// 【为什么未跟踪必须单独取】git diff 的所有变体都**只比已跟踪的路径**：
// agent 写出来的新文件在 diff 里一个字都没有，而那恰恰是最该被审的部分。
// 更糟的是它还报 added=0 / deleted=0，看起来像"这个文件没有改动"。
//
// 【为什么用 ls-files 而不是 status 的 ??】ls-files --others --exclude-standard
// 的语义最直白：**没被跟踪、也没被忽略的文件**，与索引状态无关。
std::vector<std::wstring> ParseUntrackedZ(const std::wstring& text) {
    std::vector<std::wstring> out;
    for (const std::wstring& s : SplitNul(text)) {
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- 行数统计

std::map<std::wstring, Numstat> ParseNumstatZ(const std::wstring& text) {
    std::map<std::wstring, Numstat> out;
    // 复用 SplitNul：重命名那条的"路径"是空的，但整个字段（"0\t0\t"）非空，
    // 所以不会被 SplitNul 丢掉。
    const std::vector<std::wstring> fields = SplitNul(text);
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::wstring& f = fields[i];
        const size_t t1 = f.find(L'\t');
        if (t1 == std::wstring::npos) continue;
        const size_t t2 = f.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        const std::wstring a = f.substr(0, t1);
        const std::wstring d = f.substr(t1 + 1, t2 - t1 - 1);
        const std::wstring path = f.substr(t2 + 1);
        Numstat n;
        if (a == L"-" || d == L"-") {
            n.binary = true;  // 二进制：行数无意义，如实标出来而不是给 0 冒充"没改"
        } else {
            n.added = _wtoi(a.c_str());
            n.deleted = _wtoi(d.c_str());
        }
        if (!path.empty()) {
            out[path] = n;
        } else if (i + 2 < fields.size()) {
            // 重命名：旧路径、新路径各占一个 NUL 字段；取新路径做键
            //（与 name-status 的键一致，两边才对得上）。
            out[fields[i + 2]] = n;
            i += 2;
        }
    }
    return out;
}

// ---------------------------------------------------------------- patch 解析

// 从 unified diff 的一个文件段落里取出的内容。
// 解析整份 unified diff，按文件分段。
//
// 【为什么自己解析而不直接透传 git 的 patch 文本】
//   ① 透传的话，"哪个文件改了什么"就没有结构 —— 调用方要自己再解析一遍，
//      而本仓的路径解析口径（-z、无引号）就又分叉出一份。
//   ② 未跟踪的文件**在 git 的 patch 里根本不存在**，透传永远补不上；
//      自己解析才能把新文件按"全新文件"补进去。
//
// 【路径的取法有优先级】纯重命名时 git **不发** ---/+++ 行
// （只有 diff --git + similarity index + rename from/to），只认 ---/+++ 的话
// 这一段会被整个丢掉 —— 报告里就只剩一条"没取到 diff 段落"，
// 而它其实根本没有内容差异。
std::vector<Section> ParsePatch(const std::wstring& text) {
    std::vector<Section> out;
    Section cur;
    bool inSection = false;
    bool inHunk = false;
    Hunk hunk;
    std::wstring oldPath, newPath, headerPath;

    auto flushHunk = [&]() {
        if (!inHunk) return;
        cur.hunks.push_back(hunk);
        inHunk = false;
    };
    auto flushSection = [&]() {
        flushHunk();
        //   ① `+++ b/<新路径>` —— 有内容差异时最准；
        //   ② `rename to <新路径>` —— 纯重命名时只有它；
        //   ③ `--- a/<旧路径>` —— 删除时用；
        //   ④ `diff --git` 头里的 b/ 部分 —— 只在纯模式变化时兜底。
        std::wstring p;
        if (!newPath.empty() && newPath != L"/dev/null") p = newPath;
        else if (!oldPath.empty() && oldPath != L"/dev/null") p = oldPath;
        else p = headerPath;
        if (!p.empty() && p != L"/dev/null") {
            cur.path = p;
            out.push_back(cur);
        }
        cur = Section();
        oldPath.clear();
        newPath.clear();
        headerPath.clear();
        inSection = false;
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;

        if (line.compare(0, 11, L"diff --git ") == 0) {
            flushSection();
            inSection = true;
            headerPath = HeaderPathOf(line);
            continue;
        }
        if (!inSection) continue;
        // ---/+++ 带 a/ b/ 前缀，要剥；rename/copy 的路径**不带**前缀，不能剥
        // （剥了会把一个恰好叫 "b/x.txt" 的文件变成 "x.txt"）。
        if (line.compare(0, 4, L"--- ") == 0) {
            oldPath = StripPrefix(line.substr(4));
            continue;
        }
        if (line.compare(0, 4, L"+++ ") == 0) {
            newPath = StripPrefix(line.substr(4));
            continue;
        }
        // 重命名/复制：git 把两个路径写成独立的一行（不带 a/ b/ 前缀）。
        if (line.compare(0, 11, L"rename from ") == 0) {
            oldPath = line.substr(11);
            continue;
        }
        if (line.compare(0, 9, L"rename to ") == 0) {
            newPath = line.substr(9);
            continue;
        }
        if (line.compare(0, 9, L"copy from ") == 0) {
            oldPath = line.substr(9);
            continue;
        }
        if (line.compare(0, 8, L"copy to ") == 0) {
            newPath = line.substr(8);
            continue;
        }
        if (line.compare(0, 13, L"Binary files ") == 0 ||
            line.compare(0, 8, L"Binary ") == 0) {
            cur.binary = true;
            continue;
        }
        if (line.compare(0, 3, L"@@ ") == 0) {
            flushHunk();
            Hunk h;
            if (!ParseHunkHeader(line, &h)) continue;
            h.header = line;
            hunk = h;
            inHunk = true;
            continue;
        }
        if (inHunk) {
            // hunk 正文：' ' 上下文 / '+' 新增 / '-' 删除 / '\' 无换行尾标记。
            // 空行是"空的上下文行"（git 写的是单个空格，但被某些工具裁掉过），
            // 按上下文处理，不要当成段落结束。
            if (line.empty()) {
                hunk.text += L" \n";
            } else if (line[0] == L' ' || line[0] == L'+' || line[0] == L'-' ||
                       line[0] == L'\\') {
                hunk.text += line;
                hunk.text += L"\n";
            }
        }
    }
    flushSection();
    return out;
}


}  // namespace devdiff
