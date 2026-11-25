// 评估面完整性实现（接口见 eval_surface.h）。
#include "eval_surface.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "file_hash.h"
#include "json.h"
#include "str.h"
#include "text_file.h"
#include "dev_proc.h"

namespace evalsurface {
namespace {

json::Value Str(const std::wstring& s) { return json::Value::MakeStr(s); }

// 列出一个目录下的指定扩展名的文件名（不含路径，不含 '.'/'..'）。
std::vector<std::wstring> ListExt(const std::wstring& dir, const wchar_t* ext) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = tf::Join(dir, std::wstring(L"*") + ext);
    // 长路径前缀：目录深到 260 字符以上时不加前缀会直接枚举不到。
    HANDLE h = FindFirstFileW(tf::ToExtended(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}

// 受保护目录：扩展名 → 目录。目录里的**文件集合**必须与清单完全一致。
struct DirRule {
    const wchar_t* dir;
    const wchar_t* ext;
};
constexpr DirRule kProtectedDirs[] = {
    {L"tests", L".cpp"},
    {L"tools", L".py"},
};

// 从 start 向上找仓库根（认 build_fast.py 当标志文件），最多 5 层。
std::wstring FindUpwards(std::wstring dir) {
    for (int i = 0; i < 5 && !dir.empty(); ++i) {
        if (tf::Exists(tf::Join(dir, L"build_fast.py"))) return dir;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash == 0) break;
        dir = dir.substr(0, slash);
    }
    return L"";
}

std::wstring RepoRoot() {
    // 1) exe 同级：主程序 exe 就放在仓库根，这是最常见的情况。
    const std::wstring exeDir = tf::ExeDir();
    if (tf::Exists(tf::Join(exeDir, L"build_fast.py"))) return exeDir;

    // 2) 当前工作目录：单测 exe 在临时目录里，但**从仓库根启动**
    //    （tests/run_all.sh 就是这么做的），所以 cwd 才是它的仓库根。
    wchar_t cwdBuf[MAX_PATH];
    const DWORD n = GetCurrentDirectoryW(MAX_PATH, cwdBuf);
    const std::wstring cwd = (n > 0 && n < MAX_PATH) ? std::wstring(cwdBuf, n) : L"";

    // 3) 两边都向上找。
    const std::wstring upExe = FindUpwards(exeDir);
    if (!upExe.empty()) return upExe;
    const std::wstring upCwd = FindUpwards(cwd);
    if (!upCwd.empty()) return upCwd;
    return exeDir;
}

// ---- git 信任层 ----
//
// 【为什么哈希对上了还不够】
// selfSeal 能挡住"手改清单"（改哈希但没重签），可它**挡不住"改软评估面
// + 同步重签"**：重签会算出一个**新的、正确的** selfSeal。
// 于是上一版的设计对这条最有价值的攻击路径其实是**敞开的** ——
// 而 RewardHackingAgents 的实测说，约 50% 的 episode 出现篡改 evaluator 的尝试。
//
// 真正的锚是**提交**：清单被 git 跟踪，且工作树里与 HEAD 一致。
//   · 未跟踪 → 重签不留任何痕迹（git status 里都看不见）；
//   · 已跟踪但有未提交改动 → "有人重签过还没提交"，同样不该算数。
// 人在 `git status` / code review / CI 里都能看见这两件事。
//
// 而这条约束**机械地**堵住了"AI 通道触发重签"：重签之后清单立刻变脏，
// 门仍然关着 —— 除非有人提交。本工程没有 commit 命令（N7：不提供任意写文件），
// 所以 AI 自己走不到"提交"那一步。
//
// 签发权归人这件事因此不是靠一句约定，而是靠"**重签之后门还关着**"。
struct GitTrust {
    bool ok = false;
    std::wstring reason;
};

// 在仓库根跑一条只读 git 命令，返回 (退出码, 输出)。git 不可用时 ok=false。
bool Git(const std::wstring& cwd, const std::vector<std::wstring>& args, int* code,
         std::wstring* out) {
    std::wstring err;
    if (!devproc::RunCapture(L"git", args, cwd, code, out, &err)) {
        return false;
    }
    return true;
}

GitTrust CheckGitTrust(const std::wstring& root, const std::wstring& manifestPath) {
    GitTrust t;
    // 清单路径要转成仓库相对路径（git 认的是仓库内路径）。
    std::wstring rel = manifestPath;
    const std::wstring prefix = root + L"\\";
    if (rel.compare(0, prefix.size(), prefix) == 0) rel = rel.substr(prefix.size());
    for (wchar_t& c : rel) {
        if (c == L'\\') c = L'/';
    }

    int code = 0;
    std::wstring out;
    if (!Git(root, {L"ls-files", L"--error-unmatch", rel}, &code, &out)) {
        t.reason = L"无法运行 git —— 没法确认评估面清单是否已被 git 跟踪。"
                   L"清单的信任锚是「已跟踪 + 已提交」，没有 git 就无法验证（fail-closed）。";
        return t;
    }
    if (code != 0) {
        t.reason = L"评估面清单（" + rel + L"）尚未被 git 跟踪 —— "
                   L"重签将不留任何痕迹。先执行：git add " + rel + L" 并提交。";
        return t;
    }

    out.clear();
    if (!Git(root, {L"status", L"--porcelain", L"--", rel}, &code, &out)) {
        t.reason = L"无法运行 git status —— 没法确认评估面清单有没有未提交改动（fail-closed）。";
        return t;
    }
    if (!str::Trim(out).empty()) {
        t.reason = L"评估面清单（" + rel + L"）有未提交改动 —— 有人重签过但还没提交。"
                   L"确认这次重签是有意的、并提交它：git add " + rel + L" && git commit";
        return t;
    }
    t.ok = true;
    return t;
}

// 清单正文的规范形式（与 tools/gen_eval_baseline.py 的 _canonical 逐字节一致）。
//
// 纯 ASCII 的确定行：
//     V <version>
//     F <相对路径> <sha256>
//     D <目录> <扩展名> <文件1,文件2,...>      （文件列表已排序）
// 键一律排序、分隔符是单个空格、行以 \n 结尾、整体 UTF-8。
//
// 为什么不用 json.dumps：C++ 的 json 序列化与 Python 的缩进/转义/浮点格式
// 不可能逐字节一致。两边算出的哈希一旦不同，这道门就会永远拒绝构建 ——
// 或者更糟：为让它通过而把校验删掉。
std::wstring CanonicalForm(const json::Value& manifest) {
    std::wstring canon;
    wchar_t ver[32] = L"";
    const json::Value* v = manifest.Find(L"version");
    swprintf(ver, 32, L"%lld", v && v->IsNumber() ? static_cast<long long>(v->num) : 0LL);
    canon += L"V ";
    canon += ver;
    canon += L"\n";

    const json::Value* files = manifest.Find(L"files");
    if (files && files->IsObject()) {
        for (const auto& kv : files->obj) {
            if (!kv.second.IsString()) continue;
            canon += L"F ";
            canon += kv.first;
            canon += L" ";
            canon += kv.second.str;
            canon += L"\n";
        }
    }
    const json::Value* dirSets = manifest.Find(L"dirSets");
    if (dirSets && dirSets->IsObject()) {
        for (const auto& kv : dirSets->obj) {
            const json::Value* entry = kv.second.Find(L"files");
            const json::Value* extV = kv.second.Find(L"ext");
            canon += L"D ";
            canon += kv.first;
            canon += L" ";
            canon += (extV && extV->IsString()) ? extV->str : L"";
            canon += L" ";
            bool first = true;
            if (entry && entry->IsArray()) {
                for (const json::Value& f : entry->arr) {
                    if (!f.IsString()) continue;
                    if (!first) canon += L",";
                    canon += f.str;
                    first = false;
                }
            }
            canon += L"\n";
        }
    }
    return canon;
}

bool ComputeSelfSealOf(const json::Value& manifest, std::wstring* out) {
    const std::wstring canon = CanonicalForm(manifest);
    // 统一走 str::ToUtf8（不直接调 WideCharToMultiByte）：
    // 转换规则散在多处时，"清单正文到底按什么字节算哈希"就会变得不确定 ——
    // 而 selfSeal 的全部意义就是"这个字节序列是唯一的"。
    const std::string utf8 = str::ToUtf8(canon);
    if (utf8.empty() && !canon.empty()) return false;
    return filehash::HashBytes(utf8.data(), utf8.size(), filehash::Algo::Sha256, out);
}

}  // namespace

std::wstring ManifestPath() {
    return tf::Join(tf::Join(RepoRoot(), L"data"), L"eval-surface.json");
}

Report Check() {
    Report rep;
    rep.manifestPath = ManifestPath();

    std::wstring text;
    if (!tf::ReadText(rep.manifestPath, &text)) {
        rep.manifestFound = false;
        rep.ok = false;
        rep.reason = L"评估面清单缺失：" + rep.manifestPath +
                     L"。无法确认构建器与测试未被改动，先跑："
                     L"python tools/gen_eval_baseline.py --human-confirm（并提交）。";
        return rep;
    }
    rep.manifestFound = true;

    json::Value manifest;
    std::wstring perr;
    if (!json::Parse(text, manifest, &perr) || !manifest.IsObject()) {
        rep.ok = false;
        rep.reason = L"评估面清单解析失败：" + perr + L"（" + rep.manifestPath + L"）";
        return rep;
    }

    const json::Value* files = manifest.Find(L"files");
    if (!files || !files->IsObject()) {
        rep.ok = false;
        rep.reason = L"评估面清单缺少 files 段（" + rep.manifestPath + L"）";
        return rep;
    }

    // ---- 第 0 步：清单自身未被手改（selfSeal）----
    //
    // 顺序很重要：**先验清单真伪，再拿清单去判评估面**。
    // 反过来的话，一份被改过的清单会让"评估面看起来完全正常" ——
    // 那正是最该被抓住的场景。算法与 tools/gen_eval_baseline.py 的
    // _canonical() 逐字节一致（纯 ASCII 行式，两侧都能独立复算）。
    {
        const json::Value* seal = manifest.Find(L"selfSeal");
        if (!seal || !seal->IsString() || seal->str.empty()) {
            rep.ok = false;
            rep.reason = L"评估面清单缺少 selfSeal —— 旧版签发或被人删掉了这一项。"
                         L"确认后由人重新签发：python tools/gen_eval_baseline.py --human-confirm";
            return rep;
        }
        std::wstring got;
        if (!ComputeSelfSealOf(manifest, &got)) {
            rep.ok = false;
            rep.reason = L"无法复算评估面清单的 selfSeal（系统摘要失败）";
            return rep;
        }

        if (!str::IEquals(got, seal->str)) {
            rep.ok = false;
            rep.reason = L"评估面清单正文与 selfSeal 不符 —— **清单被直接改过**（没有走重签）。"
                         L"\n这比\"评估面被改\"更可疑：改清单的人想让哪些检查看起来仍然合法？"
                         L"\n用 git diff data/eval-surface.json 看改了什么；"
                         L"确认后由人重签：python tools/gen_eval_baseline.py --human-confirm";
            return rep;
        }
    }

    // ---- 第 1 步：清单自身是否已被 git 信任（已跟踪 + 已提交）----
    //
    // 放在哈希核对**之前**：如果清单本身没人认领，那它说的任何话都不必听。
    // 这一步同时堵住"改软评估面 + 同步重签"（selfSeal 挡不住那条路，
    // 因为重签会算出**正确的**新 seal）。
    //
    // 但**不提前返回**：先把哈希差异也算出来，最后一起报 ——
    // 只报"清单没提交"而不报"哪几个文件变了"，等于让人再跑一遍才知道改了什么。
    const GitTrust trust = CheckGitTrust(RepoRoot(), rep.manifestPath);

    // ---- 逐个文件核对哈希 ----
    for (const auto& kv : files->obj) {
        const std::wstring rel = kv.first;
        const std::wstring want = kv.second.IsString() ? kv.second.str : L"";
        const std::wstring abs = tf::Join(RepoRoot(), rel);
        ++rep.checkedFiles;
        if (!tf::Exists(abs)) {
            rep.changed.push_back(rel + L"（已删除）");
            continue;
        }
        filehash::Result hr;
        if (!filehash::HashFile(abs, filehash::Algo::Sha256, &hr)) {
            rep.changed.push_back(rel + L"（读不到：" + hr.error + L"）");
            continue;
        }
        if (hr.hex != want) {
            rep.changed.push_back(rel + L"（已修改）");
        }
    }

    // ---- 受保护目录的文件集合必须完全一致 ----
    const json::Value* dirSets = manifest.Find(L"dirSets");
    if (dirSets && dirSets->IsObject()) {
        for (const auto& kv : dirSets->obj) {
            const std::wstring relDir = kv.first;
            const json::Value* entry = kv.second.Find(L"files");
            const json::Value* extV = kv.second.Find(L"ext");
            if (!entry || !entry->IsArray() || !extV || !extV->IsString()) {
                rep.changed.push_back(relDir + L"（清单里这段格式不对）");
                continue;
            }
            std::vector<std::wstring> want;
            for (const json::Value& v : entry->arr) {
                if (v.IsString()) want.push_back(v.str);
            }
            std::sort(want.begin(), want.end());
            const std::vector<std::wstring> now =
                ListExt(tf::Join(RepoRoot(), relDir), extV->str.c_str());
            for (const std::wstring& w : want) {
                if (std::find(now.begin(), now.end(), w) == now.end()) {
                    rep.changed.push_back(relDir + L"/" + w + L"（已删除）");
                }
            }
            for (const std::wstring& n : now) {
                if (std::find(want.begin(), want.end(), n) == want.end()) {
                    rep.changed.push_back(relDir + L"/" + n + L"（新增，需重新签发）");
                }
            }
            rep.checkedFiles += static_cast<int>(now.size());
        }
    }

    if (!rep.changed.empty()) {
        rep.ok = false;
        std::wstring reason = L"评估面被改动（" + std::to_wstring(rep.changed.size()) +
                              L" 项）：";
        for (size_t i = 0; i < rep.changed.size() && i < 20; ++i) {
            reason += L"\n  · " + rep.changed[i];
        }
        if (rep.changed.size() > 20) {
            reason += L"\n  · …（还有 " + std::to_wstring(rep.changed.size() - 20) + L" 项）";
        }
        reason +=
            L"\n评估器被改软时，\"测试通过\"不代表代码对。"
            L"确认改动是有意的后，跑 python tools/gen_eval_baseline.py --human-confirm 重新签发，"
            L"并把清单提交（重签之后门仍然关着，直到有人提交 —— 签发权归人）。";
        rep.reason = reason;
    }
    // 信任层的结论放在**最后**：它是"清单本身没人认领"这种更根本的问题，
    // 但把哈希差异也一并报出来，人才知道到底改了什么。
    if (!trust.ok) {
        rep.ok = false;
        rep.reason = trust.reason +
                     (rep.reason.empty() ? L"" : (L"\n另外：" + rep.reason));
    }
    return rep;
}

bool ComputeSelfSeal(const std::wstring& manifestText, std::wstring* sealOut) {
    json::Value m;
    std::wstring err;
    if (!json::Parse(manifestText, m, &err) || !m.IsObject()) return false;
    return ComputeSelfSealOf(m, sealOut);
}

std::wstring CheckJson() {
    const Report r = Check();
    json::Value v = json::Value::MakeObject();
    v.Set(L"manifestFound", json::Value::MakeBool(r.manifestFound));
    v.Set(L"ok", json::Value::MakeBool(r.ok));
    v.Set(L"manifestPath", Str(r.manifestPath));
    v.Set(L"checkedFiles", json::Value::MakeInt(r.checkedFiles));
    json::Value arr = json::Value::MakeArray();
    for (const std::wstring& c : r.changed) arr.arr.push_back(Str(c));
    v.Set(L"changed", arr);
    if (!r.reason.empty()) v.Set(L"reason", Str(r.reason));
    return json::Stringify(v, 0);
}

}  // namespace evalsurface
