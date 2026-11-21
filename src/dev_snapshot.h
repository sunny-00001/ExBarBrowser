// 开发面快照：**存内容**的工作树快照（对应报告 P0-5）。
//
// 【为什么必须存内容，而不是只存 diff 摘要】
// 旧版 dev.snapshot 只返回 head / status / diffStat —— 那是一份**摘要**，
// 看着像"留了后路"，其实什么都没留。而 dev.rollback 走的是
// `git checkout -- <file>`：工作树里那份改动就此消失，**不可逆**。
// 文档里"调用方必须先 dev.snapshot"那句话因此是**空的** ——
// 快照没存内容，snap 完再回滚，改动照样找不回来。
//
// 现在快照真的落盘：把每个改动文件的**内容副本**存进
// <repo>/snapshots/<snapshotId>/files/<相对路径>，
// 并写一份 manifest.json（清单：哪些文件、存没存下、为什么没存）。
// 回滚时按快照还原，且**回滚前先把当前内容存进快照的 pre-rollback/** ——
// 于是"回滚"这一步本身也是可逆的：可以在"快照态"与"当前态"之间来回。
//
// 【为什么不放在 data/ 下】
// 快照说的是**仓库工作树**的状态，而 dev.* 这一族命令操作的就是仓库。
// 放仓库根的 snapshots/（已 .gitignore），不污染 data/ 里那些个人配置，
// 也不会被 git 当成"新增文件"混进 status 输出。
#pragma once

#include <string>
#include <vector>

namespace devsnap {

// 快照里记录的一个文件。
struct FileEntry {
    std::wstring path;       // 仓库内相对路径（斜杠分隔）
    bool saved = false;      // 内容有没有真的存下来
    bool tracked = false;    // git 跟踪的？（未跟踪 = 快照时它就是"新文件"）
    unsigned long long bytes = 0;
    std::wstring reason;     // 没存下来的原因（如"文件太大"）
};

// 一份快照。
struct Snapshot {
    std::wstring id;         // 快照 id（也是目录名）
    std::wstring dir;        // 落盘目录
    std::wstring head;       // 快照时的 HEAD
    std::wstring createdAt;  // 人可读时间
    std::vector<FileEntry> files;
    bool ok = false;
    std::wstring error;
};

// 落盘一份快照。返回的 Snapshot 里 ok=false 表示没做成（error 给原因）。
// **不做成时不会留下半份快照**：目录建不出来就直接失败。
Snapshot Create();

// 按 id 读回快照清单。找不到返回 false。
bool Load(const std::wstring& id, Snapshot* out);

// 把文件恢复到**快照时的状态**。
//
// 语义（与旧的"恢复到 HEAD"不是一回事，调用方要清楚）：
//   · 快照里存了内容 → 写回那份副本（回到快照时的样子）；
//   · 快照时该文件是干净的（git 跟踪、没改动）→ `git checkout -- path`
//     （HEAD 的内容就是快照时的内容）；
//   · 快照时该文件**未被 git 跟踪** → 不动它（删掉新文件风险太大），
//     如实报告，让调用方自己决定。
//
// relPath 为空 = 恢复快照里记录的**全部**文件。
//
// 回滚前会把当前内容存进 <snap>/pre-rollback/<path> —— 所以这一步可逆。
// 返回 false 时 err 给原因；before/after 给内容哈希（git hash-object）。
bool Restore(const std::wstring& id, const std::wstring& relPath, std::wstring* before,
             std::wstring* after, std::wstring* err);

}  // namespace devsnap
