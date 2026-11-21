// git 输出解析层的公开接口（实现见 src/dev_diff_parse.cpp）。
//
// 这里的每个函数都是**纯函数**：给定一段 git 输出，解析结果只取决于那段输出。
// 这样才能脱离 git 单测，也才能保证"解析口径只有一份"。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "dev_diff.h"

namespace devdiff {

struct ChangeEntry {
    std::wstring status;    // added / modified / deleted / renamed / copied / typechange
    std::wstring path;      // 仓库内相对路径（重命名时是**新**路径）
    std::wstring origPath;  // 仅 renamed/copied：改名前的路径
    bool tracked = true;
};

// 把 NUL 分隔的输出拆成字段（末尾空段是分隔符，不是记录）。
struct Numstat {
    int added = 0;
    int deleted = 0;
    bool binary = false;
};

// 解析 `git diff HEAD --numstat -z`。
//
// 格式：每条是 "added\tdeleted\tpath\0"；重命名时路径字段为空，
// 后面跟两个 NUL 分隔的字段（**旧路径、新路径**，与 name-status 同向）。
// 二进制文件是 "-\t-\t"。
//
// 【为什么不复用工作区+暂存区两份 numstat 再相加】相对 HEAD 的**一份**
// numstat 就是"从上次提交到现在"的全部改动；分两份再相加，既多一次 git 调用，
// 又要在"某个文件只在一侧出现"时处理合并 —— 而相对 HEAD 一份本来就没这问题。
struct Section {
    std::wstring path;      // 解析出来的仓库内相对路径
    bool binary = false;
    std::vector<Hunk> hunks;
};

// 解析 "@@ -12,3 +12,4 @@ optional heading"。

// 解析 `git diff HEAD --name-status -z` → 改动清单。
std::vector<ChangeEntry> ParseNameStatusZ(const std::wstring& text);

// 解析 `git ls-files --others --exclude-standard -z` → 未跟踪文件清单。
std::vector<std::wstring> ParseUntrackedZ(const std::wstring& text);

// 解析 `git diff HEAD --numstat -z` → 每个路径的增删行数。
std::map<std::wstring, Numstat> ParseNumstatZ(const std::wstring& text);

// 解析整份 unified diff → 按文件分段。
std::vector<Section> ParsePatch(const std::wstring& text);

}  // namespace devdiff
