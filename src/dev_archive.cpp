// archive 落盘与读回（接口见 dev_archive.h）。
//
// 【为什么不预摘要】
// 外部消融实验显示：对执行轨迹做摘要会丢失信号。所以这里把 dev.test 的原文
// 逐字存进 trajectory/raw.txt，manifest 只记字节数和 SHA-256；
// 任何"看起来像原始但被改过"的痕迹都能靠哈希对出来。
#include "dev_archive.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "dev_diff.h"
#include "file_hash.h"
#include "json.h"
#include "str.h"
#include "text_file.h"

namespace devarchive {
namespace {

std::wstring ParentOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : path.substr(0, s);
}

std::wstring Normalize(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

// archive root 的父目录就是仓库根。
std::wstring RepoOf(const std::wstring& root) { return ParentOf(root); }

std::wstring NewId() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64] = {};
    swprintf_s(buf, L"cand-%04d%02d%02d-%02d%02d%02d-%lu", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    return buf;
}

std::wstring NowHuman() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[40] = {};
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool SafeId(const std::wstring& id) {
    if (id.empty() || id.size() > 96) return false;
    for (const wchar_t c : id) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                        (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
        if (!ok) return false;
    }
    return true;
}

bool Sha256Of(const std::string& text, std::wstring* hex) {
    return filehash::HashBytes(text.data(), text.size(), filehash::Algo::Sha256, hex);
}

json::Value ScoreToJson(const Score& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"passed", json::Value::MakeInt(s.passed));
    v.Set(L"failed", json::Value::MakeInt(s.failed));
    v.Set(L"compileFail", json::Value::MakeInt(s.compileFail));
    v.Set(L"notRun", json::Value::MakeInt(s.notRun));
    v.Set(L"value", json::Value::MakeNum(s.value));
    return v;
}

Score ScoreFromJson(const json::Value& v) {
    Score s;
    s.passed = v.GetInt(L"passed");
    s.failed = v.GetInt(L"failed");
    s.compileFail = v.GetInt(L"compileFail");
    s.notRun = v.GetInt(L"notRun");
    const json::Value* val = v.Find(L"value");
    if (val && val->IsNumber()) s.value = val->num;
    return s;
}

json::Value DecisionToJson(const DecideResult& d) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"accepted", json::Value::MakeBool(d.accepted));
    v.Set(L"decision", json::Value::MakeStr(d.decision));
    v.Set(L"eProcess", json::Value::MakeNum(d.eProcess));
    v.Set(L"threshold", json::Value::MakeNum(d.threshold));
    v.Set(L"trials", json::Value::MakeInt(d.trials));
    v.Set(L"wins", json::Value::MakeInt(d.wins));
    v.Set(L"ties", json::Value::MakeInt(d.ties));
    v.Set(L"losses", json::Value::MakeInt(d.losses));
    v.Set(L"invalid", json::Value::MakeInt(d.invalid));
    v.Set(L"reason", json::Value::MakeStr(d.reason));
    return v;
}

json::Value FunnelToJson(const FunnelResult& f) {
    json::Value arr = json::Value::MakeArray();
    for (const StageResult& s : f.stages) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"stage", json::Value::MakeStr(s.stage));
        v.Set(L"passed", json::Value::MakeBool(s.passed));
        v.Set(L"reason", json::Value::MakeStr(s.reason));
        arr.arr.push_back(std::move(v));
    }
    json::Value out = json::Value::MakeObject();
    out.Set(L"ok", json::Value::MakeBool(f.ok));
    out.Set(L"stages", std::move(arr));
    return out;
}

// 复制候选源码。只收 dev.diff 报告的文件（包括未跟踪的新文件），
// 删除项没有内容可存，如实跳过并在 manifest 里记 files 数。
std::vector<ArchivedFile> CaptureFiles(const std::wstring& root, const std::wstring& dstDir,
                                       std::wstring* err) {
    std::vector<ArchivedFile> out;
    devdiff::Options opt;
    opt.root = RepoOf(root);
    const devdiff::Report rep = devdiff::Compute(opt);
    if (!rep.ok) {
        if (err) *err = rep.error.empty() ? L"读取工作树改动失败" : rep.error;
        return out;
    }
    const std::wstring filesDir = dstDir + L"\\files";
    if (!tf::EnsureDir(filesDir)) {
        if (err) *err = L"建不了 archive/files 目录";
        return out;
    }
    for (const devdiff::FileDiff& f : rep.files) {
        if (f.status == L"deleted") continue;
        const std::wstring rel = Normalize(f.path);
        if (rel.empty() || rel.find(L"..") != std::wstring::npos ||
            rel.front() == L'\\' || rel.find(L':') != std::wstring::npos)
            continue;
        const std::wstring src = opt.root + L"\\" + rel;
        const std::wstring dst = filesDir + L"\\" + rel;
        if (!tf::EnsureDir(ParentOf(dst))) continue;
        if (!tf::CopyFileIfMissing(src, dst)) continue;
        filehash::Result h;
        if (!filehash::HashFile(dst, filehash::Algo::Sha256, &h) || !h.ok) continue;
        ArchivedFile a;
        a.path = rel;
        a.sha256 = h.hex;
        a.bytes = f.bytes;
        out.push_back(std::move(a));
    }
    return out;
}

}  // namespace

std::wstring DefaultRoot() { return tf::Join(tf::ExeDir(), L"archive"); }

RecordResult Record(const RecordOptions& opt) {
    RecordResult r;
    const std::wstring root = opt.root.empty() ? DefaultRoot() : Normalize(opt.root);
    if (!tf::EnsureDir(root)) {
        r.error = L"建不了 archive 目录：" + root;
        return r;
    }
    // id 会变成目录名：必须挡住越界字符。
    const std::wstring id = opt.id.empty() ? NewId() : opt.id;
    if (!SafeId(id)) {
        r.error = L"candidateId 不合法（只允许字母、数字、连字符与下划线）";
        return r;
    }
    if (opt.rawTrajectory.empty()) {
        r.error = L"没有原始执行轨迹（不凭空打分）";
        return r;
    }
    constexpr size_t kMaxTrajectory = 8ull * 1024 * 1024;
    if (opt.rawTrajectory.size() > kMaxTrajectory) {
        r.error = L"原始轨迹超过 8 MB（拒绝静默截断）";
        return r;
    }
    r.id = id;
    r.dir = root + L"\\" + id;
    if (tf::Exists(r.dir)) {
        r.error = L"候选已存在：" + id;
        return r;
    }

    Trajectory main = ParseTrajectory(opt.rawTrajectory);
    if (!main.ok) {
        r.error = main.error;
        return r;
    }
    r.score = main.score;

    std::vector<Trajectory> repeats;
    for (const std::string& t : opt.repeatTrajectories) {
        Trajectory p = ParseTrajectory(t);
        if (!p.ok) {
            r.error = L"重复运行轨迹不可信：" + p.error;
            return r;
        }
        repeats.push_back(std::move(p));
    }

    // 先在临时目录里做完所有事，最后一次性改名 —— 失败不留半份记录。
    const std::wstring tmp = root + L"\\." + id + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    if (tf::Exists(tmp)) {
        r.error = L"临时目录已存在：" + tmp;
        return r;
    }
    if (!tf::EnsureDir(tmp) || !tf::EnsureDir(tmp + L"\\trajectory")) {
        r.error = L"建不了临时 archive 目录";
        return r;
    }
    std::wstring copyErr;
    const std::vector<ArchivedFile> files = CaptureFiles(root, tmp, &copyErr);
    if (!copyErr.empty()) {
        r.error = copyErr;
        return r;
    }
    r.files = static_cast<int>(files.size());

    // 原始轨迹逐字写盘（UTF-8，不摘要、不截断）。
    if (!tf::WriteText(tmp + L"\\trajectory\\raw.txt", tf::FromUtf8(opt.rawTrajectory))) {
        r.error = L"写原始轨迹失败";
        return r;
    }
    std::wstring rawHash;
    if (!Sha256Of(opt.rawTrajectory, &rawHash)) {
        r.error = L"算原始轨迹哈希失败";
        return r;
    }

    std::vector<json::Value> repJson;
    for (size_t i = 0; i < opt.repeatTrajectories.size(); ++i) {
        const std::wstring name = L"repeat-" + std::to_wstring(static_cast<int>(i) + 1) + L".txt";
        if (!tf::WriteText(tmp + L"\\trajectory\\" + name,
                           tf::FromUtf8(opt.repeatTrajectories[i]))) {
            r.error = L"写重复轨迹失败：" + name;
            return r;
        }
        json::Value v = json::Value::MakeObject();
        v.Set(L"name", json::Value::MakeStr(name));
        v.Set(L"bytes", json::Value::MakeNum(
                            static_cast<double>(opt.repeatTrajectories[i].size())));
        repJson.push_back(std::move(v));
    }

    // 接受器：基线存在时读回它的原始轨迹，按测试名配对。
    r.baselineId = opt.baselineId;
    if (!opt.baselineId.empty()) {
        std::string baseRaw;
        if (!ReadTrajectory(root, opt.baselineId, L"raw", &baseRaw)) {
            r.error = L"读不到基线的原始轨迹：" + opt.baselineId;
            return r;
        }
        const Trajectory base = ParseTrajectory(baseRaw);
        if (!base.ok) {
            r.error = L"基线轨迹不可信：" + base.error;
            return r;
        }
        std::vector<Trial> trials;
        std::wstring pairErr;
        if (!BuildTrials(base, main, &trials, &pairErr)) {
            r.error = pairErr;
            return r;
        }
        r.decision = Decide(trials);
    } else {
        // 没有基线就没有配对 —— 明说"证据不足"，不给一个看着像通过的结果。
        r.decision.ok = true;
        r.decision.decision = L"insufficient";
        r.decision.reason = L"没有 baselineId，无法做配对比较";
    }
    r.funnel = RunFunnel(main, repeats, opt.protectedChanged, files);

    json::Value mf = json::Value::MakeObject();
    mf.Set(L"id", json::Value::MakeStr(id));
    mf.Set(L"createdAt", json::Value::MakeStr(NowHuman()));
    mf.Set(L"parentId", json::Value::MakeStr(opt.parentId));
    mf.Set(L"baselineId", json::Value::MakeStr(opt.baselineId));
    mf.Set(L"note", json::Value::MakeStr(opt.note));
    mf.Set(L"score", ScoreToJson(r.score));
    mf.Set(L"decision", DecisionToJson(r.decision));
    mf.Set(L"funnel", FunnelToJson(r.funnel));
    mf.Set(L"accepted", json::Value::MakeBool(r.decision.accepted && r.funnel.ok));
    json::Value filesJson = json::Value::MakeArray();
    for (const ArchivedFile& f : files) {
        json::Value v = json::Value::MakeObject();
        v.Set(L"path", json::Value::MakeStr(f.path));
        v.Set(L"sha256", json::Value::MakeStr(f.sha256));
        v.Set(L"bytes", json::Value::MakeNum(static_cast<double>(f.bytes)));
        filesJson.arr.push_back(std::move(v));
    }
    mf.Set(L"files", std::move(filesJson));
    json::Value traj = json::Value::MakeObject();
    traj.Set(L"name", json::Value::MakeStr(L"raw.txt"));
    traj.Set(L"bytes", json::Value::MakeNum(static_cast<double>(opt.rawTrajectory.size())));
    traj.Set(L"sha256", json::Value::MakeStr(rawHash));
    mf.Set(L"trajectory", std::move(traj));
    json::Value reps = json::Value::MakeArray();
    for (json::Value& v : repJson) reps.arr.push_back(std::move(v));
    mf.Set(L"repeats", std::move(reps));

    if (!tf::WriteText(tmp + L"\\manifest.json", json::Stringify(mf, 2))) {
        r.error = L"写 manifest 失败";
        return r;
    }
    if (!MoveFileW(tf::ToExtended(tmp).c_str(), tf::ToExtended(r.dir).c_str())) {
        r.error = L"提交 archive 目录失败（临时目录保留：" + tmp + L"）";
        return r;
    }
    r.ok = true;
    return r;
}

bool ReadTrajectory(const std::wstring& rootIn, const std::wstring& id,
                    const std::wstring& name, std::string* out) {
    if (!SafeId(id)) return false;
    if (name.find(L"..") != std::wstring::npos || name.find(L"\\") != std::wstring::npos ||
        name.find(L"/") != std::wstring::npos)
        return false;
    const std::wstring root = rootIn.empty() ? DefaultRoot() : Normalize(rootIn);
    std::wstring wide;
    if (!tf::ReadText(root + L"\\" + id + L"\\trajectory\\" + name + L".txt", &wide))
        return false;
    if (out) *out = tf::ToUtf8(wide);
    return true;
}

ListResult List(const std::wstring& rootIn) {
    ListResult out;
    const std::wstring root = rootIn.empty() ? DefaultRoot() : Normalize(rootIn);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(tf::ToExtended(root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::wstring id = fd.cFileName;
        if (id == L"." || id == L".." || !SafeId(id)) continue;
        std::wstring text;
        if (!tf::ReadText(root + L"\\" + id + L"\\manifest.json", &text)) {
            out.skipped.push_back(id);
            continue;
        }
        json::Value v;
        std::wstring err;
        if (!json::Parse(text, v, &err) || !v.IsObject()) {
            out.skipped.push_back(id);
            continue;
        }
        Entry e;
        e.id = v.GetStr(L"id");
        e.createdAt = v.GetStr(L"createdAt");
        e.parentId = v.GetStr(L"parentId");
        e.baselineId = v.GetStr(L"baselineId");
        e.accepted = v.GetBool(L"accepted");
        const json::Value* score = v.Find(L"score");
        if (score) e.score = ScoreFromJson(*score);
        const json::Value* decision = v.Find(L"decision");
        if (decision && decision->Find(L"eProcess") &&
            decision->Find(L"eProcess")->IsNumber())
            e.eProcess = decision->Find(L"eProcess")->num;
        const json::Value* funnel = v.Find(L"funnel");
        if (funnel) e.funnelOk = funnel->GetBool(L"ok");
        const json::Value* files = v.Find(L"files");
        if (files && files->IsArray()) e.files = static_cast<int>(files->arr.size());
        out.entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // 新的在前（id 里带时间戳，字典序即时间序）。
    std::sort(out.entries.begin(), out.entries.end(),
              [](const Entry& a, const Entry& b) { return a.id > b.id; });
    return out;
}

}  // namespace devarchive
