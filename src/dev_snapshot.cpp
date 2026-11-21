// 开发面快照的实现（接口说明见 dev_snapshot.h）。
#include "dev_snapshot.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common.h"
#include "dev_proc.h"
#include "file_hash.h"
#include "json.h"
#include "str.h"
#include "text_file.h"

namespace devsnap {
namespace {

// 单文件上限 4 MB、单份快照总量上限 64 MB。
//
// 为什么有上限：快照是**兜底**，不是备份。agent 的工作树里可能有
// 编译产物、模型文件、压缩包 —— 整个拷一遍会把磁盘塞满，
// 而真正需要救回来的永远是源码（几十 KB）。
constexpr unsigned long long kMaxFileBytes = 4ull * 1024 * 1024;
constexpr unsigned long long kMaxTotalBytes = 64ull * 1024 * 1024;

std::wstring RepoRoot() { return tf::ExeDir(); }

std::wstring SnapshotsRoot() { return RepoRoot() + L"\\snapshots"; }

std::wstring SnapshotDir(const std::wstring& id) { return SnapshotsRoot() + L"\\" + id; }

// 递归建目录。返回 true = 已存在或建成功。
bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.empty() || cur.size() <= 2) {   // "C:" 之类不建
                if (i < dir.size()) cur += dir[i];
                continue;
            }
            // 长路径：CreateDirectoryW 同样受 MAX_PATH 限制，
            // 深目录（>260 字符）必须走扩展前缀，否则建不出来。
            if (!CreateDirectoryW(tf::ToExtended(cur).c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        if (i < dir.size()) cur += dir[i];
    }
    return true;
}

std::wstring NowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

std::wstring NowHuman() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

// 在仓库目录里跑一条 git 命令（只读）。不经 shell（理由见 dev_proc.h）。
std::wstring GitArgs(const std::vector<std::wstring>& args) {
    int code = 0;
    std::wstring out;
    devproc::RunCapture(L"git", args, RepoRoot(), &code, &out, nullptr);
    return out;
}

std::wstring Git(const std::wstring& sub, const std::vector<std::wstring>& rest) {
    std::vector<std::wstring> args;
    args.push_back(sub);
    for (const std::wstring& a : rest) args.push_back(a);
    return GitArgs(args);
}

// 仓库内相对路径 → 绝对路径。拒绝越出仓库的路径。
std::wstring AbsOf(const std::wstring& rel) {
    std::wstring r = rel;
    for (auto& c : r)
        if (c == L'/') c = L'\\';
    return RepoRoot() + L"\\" + r;
}

// git 跟踪的文件清单（用于判断"快照时它是不是新文件"）。
std::vector<std::wstring> TrackedFiles() {
    std::vector<std::wstring> out;
    const std::wstring text = Git(L"ls-files", {});
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) out.push_back(line);
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

// 改动文件清单（git status --porcelain 的第一列/第二列都算）。
//
// 用 `-c core.quotepath=false`：默认 git 会把非 ASCII 路径转义成
// "a\346\226\207.txt" 这种八进制串 —— 拿它当路径去拷文件必然失败。
// 加了它就是原始路径。
std::vector<std::wstring> ChangedFiles() {
    std::vector<std::wstring> out;
    // 注意参数顺序：`git -c core.quotepath=false status --porcelain`
    // —— `-c` 是 git 的**全局**选项，必须排在子命令**前面**。
    // 写成 `git status --porcelain -c ...` 会被 git 当成 status 的子命令参数，
    // 结果是打印一段用法说明（实测踩过：整份快照里全是 "unknown switch"）。
    const std::wstring text = GitArgs({L"-c", L"core.quotepath=false", L"status", L"--porcelain"});
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (line.size() > 3) {
            std::wstring path = line.substr(3);
            // 重命名行是 "new -> old"，取新的那个。
            const size_t arrow = path.find(L" -> ");
            if (arrow != std::wstring::npos) path = path.substr(0, arrow);
            while (!path.empty() && (path.back() == L'\r')) path.pop_back();
            if (!path.empty() && path.front() != L'"') out.push_back(path);
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

unsigned long long FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(tf::ToExtended(path).c_str(), GetFileExInfoStandard, &attr))
        return 0;
    return (static_cast<unsigned long long>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
}

// 文件的最后写入时间（十进制 Unix 秒，字符串形态）。返回空串 = 取不到，不编造。
// 为什么用字符串而不是整数：JSON 的 Value 只有 GetInt（32 位），Unix 秒 2038 年溢出；
// 字符串既能精确表达大整数，人读清单时也一眼能看出，还免去 double 精度问题。
// 为什么是 UTC Unix 秒而不是本地格式串：调用方要的是**可比较**的时间戳，
// 本地格式串在不同时区/语言下不可比，机器也无法拿它做"哪份更新"的判断。
std::wstring FileMtimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(tf::ToExtended(path).c_str(), GetFileExInfoStandard, &attr))
        return L"";
    // FILETIME 是 1601-01-01 起、100ns 为单位；Unix 是 1970-01-01 起、秒为单位。
    ULARGE_INTEGER ticks;
    ticks.LowPart = attr.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    constexpr unsigned long long kEpochOffset = 116444736000000000ULL;
    constexpr unsigned long long kTicksPerSecond = 10000000ULL;
    if (ticks.QuadPart < kEpochOffset) return L"";
    return std::to_wstring((ticks.QuadPart - kEpochOffset) / kTicksPerSecond);
}

// 文件内容摘要（SHA-256 小写 hex）。空文件也如实算（哈希空串有确定值），
// 取不到（被独占/读失败）返回空串 —— 空串的含义是"没算出来"，不是"内容是空的"。
std::wstring Sha256Of(const std::wstring& path) {
    filehash::Result r;
    if (!filehash::HashFile(path, filehash::Algo::Sha256, &r)) return L"";
    return r.ok ? r.hex : L"";
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(tf::ToExtended(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 复制文件（建目录 + CopyFileW）。
bool CopyFileTo(const std::wstring& src, const std::wstring& dst) {
    const std::wstring dir = ParentOf(dst);
    if (!dir.empty() && !EnsureDir(dir)) return false;
    // CopyFileW 同样受 MAX_PATH 限制：源与目标都要走扩展前缀。
    return CopyFileW(tf::ToExtended(src).c_str(), tf::ToExtended(dst).c_str(), FALSE) != FALSE;
}

std::wstring HashOf(const std::wstring& rel) {
    std::wstring out;
    int code = 0;
    devproc::RunCapture(L"git", {L"hash-object", rel}, RepoRoot(), &code, &out, nullptr);
    return str::Trim(out);
}

std::wstring ManifestPath(const std::wstring& dir) { return dir + L"\\manifest.json"; }

std::wstring FilesDir(const std::wstring& dir) { return dir + L"\\files"; }

std::wstring PreRollbackDir(const std::wstring& dir) { return dir + L"\\pre-rollback"; }

json::Value ToJson(const Snapshot& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"id", json::Value::MakeStr(s.id));
    v.Set(L"dir", json::Value::MakeStr(s.dir));
    v.Set(L"head", json::Value::MakeStr(s.head));
    v.Set(L"createdAt", json::Value::MakeStr(s.createdAt));
    json::Value arr = json::Value::MakeArray();
    for (const auto& f : s.files) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"path", json::Value::MakeStr(f.path));
        o.Set(L"saved", json::Value::MakeBool(f.saved));
        o.Set(L"tracked", json::Value::MakeBool(f.tracked));
        o.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
        if (!f.reason.empty()) o.Set(L"reason", json::Value::MakeStr(f.reason));
        o.Set(L"mtime", json::Value::MakeStr(f.mtime));
        o.Set(L"sha256", json::Value::MakeStr(f.sha256));
        arr.arr.push_back(std::move(o));
    }
    v.Set(L"files", arr);
    return v;
}

bool FromJson(const json::Value& v, Snapshot* out) {
    if (!v.IsObject()) return false;
    Snapshot s;
    s.id = v.GetStr(L"id");
    s.dir = v.GetStr(L"dir");
    s.head = v.GetStr(L"head");
    s.createdAt = v.GetStr(L"createdAt");
    s.ok = true;
    const json::Value* files = v.Find(L"files");
    if (files && files->IsArray()) {
        for (const json::Value& o : files->arr) {
            FileEntry f;
            f.path = o.GetStr(L"path");
            f.saved = o.GetBool(L"saved");
            f.tracked = o.GetBool(L"tracked");
            f.bytes = static_cast<unsigned long long>(o.GetInt(L"bytes"));
            f.reason = o.GetStr(L"reason");
            // 旧快照没有这两个字段 → GetStr 返回空串，正好是"未知"的诚实表示。
            f.mtime = o.GetStr(L"mtime");
            f.sha256 = o.GetStr(L"sha256");
            s.files.push_back(std::move(f));
        }
    }
    if (out) *out = std::move(s);
    return true;
}

}  // namespace

Snapshot Create() {
    Snapshot s;
    s.head = str::Trim(Git(L"rev-parse", {L"--short", L"HEAD"}));
    s.createdAt = NowHuman();
    // id 里带进程号：同一秒里连做两次快照也不会撞目录。
    s.id = L"snap-" + NowStamp() + L"-" + std::to_wstring(GetCurrentProcessId());
    s.dir = SnapshotDir(s.id);

    if (!EnsureDir(SnapshotsRoot())) {
        s.error = L"建不了快照目录：" + SnapshotsRoot();
        return s;
    }
    if (!EnsureDir(FilesDir(s.dir))) {
        s.error = L"建不了快照目录：" + FilesDir(s.dir);
        return s;
    }

    const std::vector<std::wstring> tracked = TrackedFiles();
    const std::vector<std::wstring> changed = ChangedFiles();
    unsigned long long total = 0;
    for (const std::wstring& rel : changed) {
        FileEntry f;
        f.path = rel;
        f.tracked = std::find(tracked.begin(), tracked.end(), rel) != tracked.end();
        const std::wstring abs = AbsOf(rel);
        f.bytes = FileSizeOf(abs);
        // P0-9：mtime 对**所有**存在的文件都取（含没保存内容的），
        // sha256 只对真的复制下来的文件算 —— 没存的那些没有内容副本可校验。
        f.mtime = FileMtimeOf(abs);
        if (IsDirectory(abs)) {
            f.reason = L"目录（不复制内容）";
            s.files.push_back(std::move(f));
            continue;
        }
        if (!f.tracked && !f.bytes) {
            f.reason = L"文件已不存在";
            s.files.push_back(std::move(f));
            continue;
        }
        if (f.bytes > kMaxFileBytes) {
            f.reason = L"文件太大（超过单文件上限，未保存）";
            s.files.push_back(std::move(f));
            continue;
        }
        if (total + f.bytes > kMaxTotalBytes) {
            f.reason = L"超过单份快照总量上限，未保存";
            s.files.push_back(std::move(f));
            continue;
        }
        const std::wstring dst = FilesDir(s.dir) + L"\\" + rel;
        if (!CopyFileTo(abs, dst)) {
            f.reason = L"复制失败";
            s.files.push_back(std::move(f));
            continue;
        }
        // 摘要算**磁盘上那份副本**，不是内存里的既有内容 —— 与快照落盘的字节一致，
        // 调用方拿这个 sha256 和 tool.hash 的结果对比才是同一个东西。
        f.sha256 = Sha256Of(dst);
        f.saved = true;
        total += f.bytes;
        s.files.push_back(std::move(f));
    }

    const std::wstring manifest = json::Stringify(ToJson(s), 2);
    if (!tf::WriteText(ManifestPath(s.dir), manifest)) {
        s.error = L"写清单失败：" + ManifestPath(s.dir);
        s.ok = false;
        return s;
    }
    s.ok = true;
    return s;
}

std::vector<FileEntry> CurrentChanges() {
    const std::vector<std::wstring> tracked = TrackedFiles();
    std::vector<FileEntry> out;
    for (const std::wstring& rel : ChangedFiles()) {
        FileEntry f;
        f.path = rel;
        f.tracked = std::find(tracked.begin(), tracked.end(), rel) != tracked.end();
        const std::wstring abs = AbsOf(rel);
        f.bytes = FileSizeOf(abs);
        f.mtime = FileMtimeOf(abs);
        // 与 Create() 分工：这里**不落盘**，但 SHA-256 算的是当前工作树那份文件本身。
        // 目录跳过（HashFile 对目录返回失败）；删除了的文件 bytes=0，也跳过。
        if (!IsDirectory(abs) && f.bytes) f.sha256 = Sha256Of(abs);
        out.push_back(std::move(f));
    }
    return out;
}

bool Load(const std::wstring& id, Snapshot* out) {
    if (id.empty()) return false;
    // id 会变成目录名 —— 必须挡住越界（".."、盘符、斜杠）。
    for (const wchar_t c : id) {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'.')
            return false;
    }
    const std::wstring dir = SnapshotDir(id);
    std::wstring text;
    if (!tf::ReadText(ManifestPath(dir), &text)) return false;
    json::Value v;
    std::wstring err;
    if (!json::Parse(text, v, &err) || !v.IsObject()) return false;
    return FromJson(v, out);
}

bool Restore(const std::wstring& id, const std::wstring& relPath, std::wstring* before,
             std::wstring* after, std::wstring* err) {
    Snapshot s;
    if (!Load(id, &s)) {
        if (err) *err = L"找不到快照：" + id;
        return false;
    }
    // 要恢复哪些文件：指定了就只恢复那一个；没指定就恢复快照里全部。
    std::vector<FileEntry> targets;
    for (const auto& f : s.files) {
        if (!relPath.empty() && f.path != relPath) continue;
        targets.push_back(f);
    }
    if (targets.empty()) {
        if (err)
            *err = relPath.empty() ? L"快照里没有记录任何文件"
                                   : L"快照里没有这个文件：" + relPath;
        return false;
    }

    std::wstring beforeHashes, afterHashes;
    int failed = 0;
    std::wstring firstError;
    for (const auto& f : targets) {
        const std::wstring abs = AbsOf(f.path);
        // ① 先把**当前**内容存进快照的 pre-rollback/ —— 这一步让回滚可逆。
        if (FileSizeOf(abs)) {
            const std::wstring keep = PreRollbackDir(s.dir) + L"\\" + f.path;
            if (!CopyFileTo(abs, keep) && firstError.empty())
                firstError = L"当前内容没能存进快照（这一步不可逆）：" + f.path;
        }
        const std::wstring beforeHash = HashOf(f.path);

        bool ok = false;
        if (f.saved) {
            // 快照里有内容副本 → 写回它（回到快照时的样子）。
            ok = CopyFileTo(FilesDir(s.dir) + L"\\" + f.path, abs);
            if (!ok && firstError.empty())
                firstError = L"写回快照内容失败：" + f.path;
        } else if (f.tracked) {
            // 快照时它是干净的 → HEAD 的内容就是快照时的内容。
            int code = 0;
            std::wstring out;
            devproc::RunCapture(L"git", {L"checkout", L"--", f.path}, RepoRoot(), &code, &out,
                                nullptr);
            ok = (code == 0);
            if (!ok && firstError.empty())
                firstError = L"git checkout 失败：" + f.path;
        } else {
            // 快照时它就不是 git 跟踪的文件（多半是新建的）。
            // 删掉风险太大 —— 如实报告，让调用方自己决定。
            if (firstError.empty())
                firstError = L"该文件在快照时未被 git 跟踪，未做处理：" + f.path;
            ok = false;
        }
        const std::wstring afterHash = HashOf(f.path);
        if (!beforeHashes.empty()) beforeHashes += L";";
        if (!afterHashes.empty()) afterHashes += L";";
        beforeHashes += beforeHash;
        afterHashes += afterHash;
        if (!ok) ++failed;
    }
    if (before) *before = beforeHashes;
    if (after) *after = afterHashes;
    if (failed) {
        if (err)
            *err = firstError.empty()
                       ? L"部分文件没能恢复（" + std::to_wstring(failed) + L" 项）"
                       : firstError;
        return false;
    }
    return true;
}

}  // namespace devsnap
