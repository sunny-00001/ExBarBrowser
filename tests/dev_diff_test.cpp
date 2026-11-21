// 结构化 diff 引擎的**内容级**单测：在临时 git 仓库里造真实场景。
//
// 【为什么必须是内容级】形状断言（"返回了 files[]"、"每个文件有 path 字段"）
// **测不出**这一层真正的缺陷 —— 那些缺陷的表现恰恰是
// "字段齐全但内容不对/缺项"：
//   · 未跟踪的新文件在 git diff 里根本不存在 → 少一项，形状照样对；
//   · 重命名取到旧路径 → path 有值，只是取错了；
//   · 纯重命名没有 ---/+++ 行 → 段落被丢掉，报"没取到"；
//   · 清单用 status 口径、内容用 diff HEAD 口径 → 两边对不上。
// 所以这里在**真仓库**里造"修改 + 未跟踪新文件 + 纯重命名 + 删除 + 空格文件名"，
// 然后逐项核对**看到了什么**。
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "dev_diff.h"
#include "dev_proc.h"
#include "text_file.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Expect(bool c, const char* what) {
    if (c) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

// 临时仓库根（带进程号，避免多会话互相踩）。
std::wstring TempRoot() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    const std::wstring base = n ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"eb_dev_diff_" + std::to_wstring(GetCurrentProcessId());
}

// 跑一条 git 命令。返回是否成功（退出码 0）。
bool Git(const std::wstring& cwd, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> full;
    full.push_back(L"-c");
    full.push_back(L"core.quotepath=false");
    for (const std::wstring& a : args) full.push_back(a);
    int code = 0;
    std::wstring out, err;
    bool overflow = false;
    if (!devproc::RunCapture(L"git", full, cwd, &code, &out, &err, &overflow)) return false;
    return code == 0;
}

// 在仓库里写一个文件（内容按 UTF-8 写出，行尾用 \n）。
bool WriteRel(const std::wstring& root, const std::wstring& rel, const std::wstring& text) {
    const std::wstring abs = root + L"\\" + rel;
    return tf::WriteText(abs, text);
}

// 找到某个 path 的条目；找不到返回 nullptr。
const devdiff::FileDiff* Find(const devdiff::Report& rep, const wchar_t* path) {
    for (const devdiff::FileDiff& f : rep.files) {
        if (f.path == path) return &f;
    }
    return nullptr;
}

// 整目录删掉（沿用 disk_scan_test 的做法）。
void RemoveTree(const std::wstring& root) {
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());
}

}  // namespace

int main() {
    std::printf("结构化 diff 引擎（内容级）\n");

    const std::wstring root = TempRoot();
    RemoveTree(root);
    tf::EnsureDir(root);

    // ---- 造场景 ----
    Git(root, {L"init", L"-q", L"."});
    Git(root, {L"config", L"user.email", L"t@example.com"});
    Git(root, {L"config", L"user.name", L"t"});
    Git(root, {L"config", L"core.autocrlf", L"false"});

    WriteRel(root, L"a.txt", L"l1\nl2\nl3\n");
    WriteRel(root, L"b.txt", L"x\ny\n");
    WriteRel(root, L"c.txt", L"m\n");
    Git(root, {L"add", L"-A"});
    Git(root, {L"commit", L"-q", L"-m", L"init"});

    // 修改 a.txt（改 1 行 + 加 1 行）
    WriteRel(root, L"a.txt", L"l1\nCHANGED\nl3\nl4\n");
    // 纯重命名 b.txt -> renamed.txt（**内容不变**）
    Git(root, {L"mv", L"b.txt", L"renamed.txt"});
    // 删除 c.txt
    DeleteFileW((root + L"\\c.txt").c_str());
    // 未跟踪新文件；文件名**带空格**（非 -z 输出会把它加引号）
    WriteRel(root, L"my new file.txt", L"u1\nu2\n");

    // ---- 算 diff ----
    devdiff::Options opt;
    opt.root = root;
    const devdiff::Report rep = devdiff::Compute(opt);

    Expect(rep.ok, "仓库可用时 diff 成功");
    Expect(rep.error.empty(), "成功时不带错误信息");
    Expect(!rep.head.empty(), "带短 HEAD");
    Expect(rep.totalFiles == 4, "改动文件总数 = 4（改/改名/删/未跟踪）");

    // ---- 修改的文件 ----
    const devdiff::FileDiff* a = Find(rep, L"a.txt");
    Expect(a != nullptr, "修改的文件在清单里");
    if (a) {
        Expect(a->status == L"modified", "a.txt 状态是 modified");
        // l2 -> CHANGED（-1 +1），末尾多一行 l4（+1）→ 增 2 删 1。
        Expect(a->added == 2 && a->deleted == 1, "a.txt 增删行数 = 2/1");
        Expect(a->tracked, "a.txt 是已跟踪文件");
        Expect(!a->sha256.empty(), "a.txt 有内容摘要");
        Expect(!a->mtime.empty(), "a.txt 有修改时间");
        Expect(a->hunks.size() == 1, "a.txt 有一段 hunk");
        bool sawChange = false;
        for (const devdiff::Hunk& h : a->hunks) {
            if (h.text.find(L"+CHANGED") != std::wstring::npos &&
                h.text.find(L"-l2") != std::wstring::npos) {
                sawChange = true;
            }
        }
        Expect(sawChange, "a.txt 的 hunk 里能看到改动的两行");
        Expect(!a->truncated, "a.txt 不该被标成截断");
    }

    // ---- 未跟踪的新文件（旧实现完全看不见它）----
    const devdiff::FileDiff* n = Find(rep, L"my new file.txt");
    Expect(n != nullptr, "未跟踪的新文件在清单里（旧实现这里是空的）");
    if (n) {
        Expect(n->status == L"untracked", "新文件状态是 untracked");
        Expect(!n->tracked, "新文件标记为未跟踪");
        Expect(n->added == 2 && n->deleted == 0, "新文件行数 = 2");
        Expect(n->hunks.size() == 1, "新文件合成了一段 hunk");
        bool sawContent = false;
        for (const devdiff::Hunk& h : n->hunks) {
            if (h.text.find(L"+u1") != std::wstring::npos &&
                h.text.find(L"+u2") != std::wstring::npos) {
                sawContent = true;
            }
        }
        Expect(sawContent, "新文件的内容**可见**（旧实现一个字都看不到）");
        Expect(!n->sha256.empty(), "新文件有内容摘要");
    }

    // ---- 纯重命名（git 不发 ---/+++ 行，旧解析会整个丢掉）----
    const devdiff::FileDiff* r = Find(rep, L"renamed.txt");
    Expect(r != nullptr, "重命名后的新路径在清单里");
    if (r) {
        Expect(r->status == L"renamed", "状态是 renamed");
        Expect(r->origPath == L"b.txt", "记下了改名前的路径");
        Expect(!r->truncated, "纯重命名不该被标成截断（本来就没有行级差异）");
        Expect(r->hunks.empty(), "纯重命名没有行级差异");
    }
    Expect(Find(rep, L"b.txt") == nullptr, "旧路径不单独出现（它是同一条改动的另一半）");

    // ---- 删除 ----
    const devdiff::FileDiff* d = Find(rep, L"c.txt");
    Expect(d != nullptr, "删除的文件在清单里");
    if (d) {
        Expect(d->status == L"deleted", "状态是 deleted");
        Expect(d->deleted == 1 && d->added == 0, "删除行数 = 1");
    }

    // ---- 路径过滤 ----
    {
        devdiff::Options o2;
        o2.root = root;
        o2.onlyPath = L"a.txt";
        const devdiff::Report r2 = devdiff::Compute(o2);
        Expect(r2.ok && r2.files.size() == 1, "只给一个文件时只返回那一个");
    }
    {
        // 用**旧路径**问重命名的文件，也要问得到
        devdiff::Options o3;
        o3.root = root;
        o3.onlyPath = L"b.txt";
        const devdiff::Report r3 = devdiff::Compute(o3);
        Expect(r3.ok && r3.files.size() == 1, "用改名前的路径也能问到");
    }
    {
        // 反斜杠写法也要能命中（Windows 习惯）
        devdiff::Options o4;
        o4.root = root;
        o4.onlyPath = L"my new file.txt";
        const devdiff::Report r4 = devdiff::Compute(o4);
        Expect(r4.ok && r4.files.size() == 1, "带空格与路径分隔的过滤能命中");
    }
    {
        devdiff::Options o5;
        o5.root = root;
        o5.onlyPath = L"no-such-file.txt";
        const devdiff::Report r5 = devdiff::Compute(o5);
        Expect(r5.ok && r5.files.empty() && !r5.note.empty(),
               "问一个没改动的文件：空清单 + 一句说明（不是\"报错\"）");
    }

    // ---- 仓库不可用时**不许编数据** ----
    {
        devdiff::Options o6;
        o6.root = TempRoot() + L"_not_a_repo";
        tf::EnsureDir(o6.root);
        const devdiff::Report r6 = devdiff::Compute(o6);
        Expect(!r6.ok && !r6.error.empty(), "不在仓库里：报错，不返回空 diff");
        Expect(r6.files.empty(), "失败时不返回任何文件项");
        RemoveTree(o6.root);
    }

    RemoveTree(root);
    std::printf("  通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
