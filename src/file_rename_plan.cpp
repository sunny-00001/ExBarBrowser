// 批量改名的"计划" —— 纯文本，不碰磁盘。
//
// 【为什么要单独成文件】批量改名最容易毁掉一个目录，而毁掉的方式都是
// "算错了名字"而不是"调用失败"：重名、撞到已有文件、生成空名/非法字符。
// 这些判定必须在**动手之前**全部做完，而且必须能脱离窗口测。
//
// 它与查重/比较（file_tools.cpp）没有共用逻辑：那两组读文件内容，
// 这一组只做字符串运算。放在一起会让"改名计划"淹没在遍历与哈希的代码里。
#include "file_rename_plan.h"

#include <algorithm>
#include <set>

namespace filetools {
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
