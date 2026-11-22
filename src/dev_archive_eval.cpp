// 接受器 / 轨迹解析 / validity funnel 的实现（接口见 dev_archive.h）。
//
// 这里刻意不碰磁盘：判定与解析是纯函数，蒙特卡洛检验才能直接跑它们，
// 也才能证明"H0 下假接受率 ≤ α"不是一句写在注释里的愿望。
#include "dev_archive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "text_file.h"

namespace devarchive {
namespace {

const wchar_t* const kStages[] = {L"parse", L"prescan", L"compile", L"checks", L"stable"};

int SignOf(double candidate, double baseline) {
    if (candidate > baseline) return 1;
    if (candidate < baseline) return -1;
    return 0;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 只接受十进制数字串，避免 atoi 把 "12x3" 读成 12。
bool ParseCount(const std::string& s, size_t pos, int* out) {
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
    long long v = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10 + (s[pos] - '0');
        ++pos;
        if (v > 1000000000) return false;
    }
    *out = static_cast<int>(v);
    return true;
}

// 从汇总行取 4 个计数。找不到/数字不全 → false（不猜测）。
bool ParseSummary(const std::string& raw, int* passed, int* notRun, int* failed,
                  int* compileFail) {
    const std::string marks[] = {"通过 ", "无法运行 ", "运行失败 ", "编译失败 "};
    size_t at = std::string::npos;
    for (size_t i = 0; i < raw.size();) {
        size_t nl = raw.find('\n', i);
        if (nl == std::string::npos) nl = raw.size();
        const std::string line = raw.substr(i, nl - i);
        bool all = true;
        for (const std::string& m : marks)
            if (line.find(m) == std::string::npos) all = false;
        if (all) at = i;  // 取最后一条：前面的进度文案不该影响对账
        if (nl == raw.size()) break;
        i = nl + 1;
    }
    if (at == std::string::npos) return false;
    size_t nl = raw.find('\n', at);
    if (nl == std::string::npos) nl = raw.size();
    const std::string line = raw.substr(at, nl - at);
    int* dst[4] = {passed, notRun, failed, compileFail};
    for (int i = 0; i < 4; ++i) {
        const size_t p = line.find(marks[i]);
        if (!ParseCount(line, p + marks[i].size(), dst[i])) return false;
    }
    return true;
}

// 逐测试结果是否完全一致（名字 + 状态），用于 stable rollout。
bool SameOutcomes(const std::vector<TestOutcome>& a, const std::vector<TestOutcome>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].status != b[i].status) return false;
    return true;
}

}  // namespace

DecideResult Decide(const std::vector<Trial>& trials) {
    DecideResult r;
    r.threshold = kThreshold;
    r.eProcess = 1.0;
    r.trace.push_back(1.0);

    // null 试验不能接受：没有证据不是"证据支持"。
    if (trials.empty()) {
        r.ok = true;
        r.decision = L"insufficient";
        r.reason = L"没有配对试验，无法判定（空集不是证据）";
        return r;
    }

    for (const Trial& t : trials) {
        if (!t.valid) {
            ++r.invalid;
            continue;
        }
        ++r.trials;
        const int x = SignOf(t.candidate, t.baseline);
        if (x > 0) ++r.wins;
        else if (x < 0) ++r.losses;
        else ++r.ties;
        // 下注因子：1 + λX。λ 是编译期常量（predictable）；
        // 1-λ = 0.5 > 0，所以 E 永远为正，Ville 不等式适用。
        r.eProcess *= (1.0 + kLambda * static_cast<double>(x));
        r.trace.push_back(r.eProcess);
    }

    r.ok = true;
    // 不可用配对一律不接受：把没跑完的试验当 0 或丢掉，
    // 就是 PACE 指出的"伪自修改"入口。
    if (r.invalid > 0) {
        r.decision = L"reject";
        r.reason = L"有 " + std::to_wstring(r.invalid) +
                   L" 对试验不可用（超时/轨迹不一致），拒绝接受";
        return r;
    }
    if (r.trials == 0) {
        r.decision = L"insufficient";
        r.reason = L"没有有效配对试验";
        return r;
    }
    if (r.eProcess >= r.threshold) {
        r.accepted = true;
        r.decision = L"accept";
        r.reason = L"配对 e-process E=" + std::to_wstring(r.eProcess) +
                   L" ≥ 1/α=" + std::to_wstring(r.threshold);
    } else {
        r.decision = L"reject";
        r.reason = L"配对 e-process E=" + std::to_wstring(r.eProcess) +
                   L" < 1/α=" + std::to_wstring(r.threshold) +
                   L"；分数涨了不等于改进成立";
    }
    return r;
}

DecideResult Decide(const json::Value& trialsJson) {
    if (!trialsJson.IsArray()) {
        DecideResult r;
        r.reason = L"trials 必须是数组";
        return r;
    }
    std::vector<Trial> trials;
    for (const json::Value& v : trialsJson.arr) {
        if (!v.IsObject()) {
            DecideResult r;
            r.reason = L"trials 的元素必须是对象";
            return r;
        }
        const json::Value* c = v.Find(L"candidate");
        const json::Value* b = v.Find(L"baseline");
        if (!c || !b || !c->IsNumber() || !b->IsNumber()) {
            DecideResult r;
            r.reason = L"每对试验必须有数值 candidate 与 baseline";
            return r;
        }
        Trial t;
        t.task = v.GetStr(L"task");
        t.candidate = c->num;
        t.baseline = b->num;
        trials.push_back(std::move(t));
    }
    return Decide(trials);
}

Trajectory ParseTrajectory(const std::string& raw) {
    Trajectory r;
    r.raw = raw;
    std::map<std::wstring, TestStatus> seen;
    size_t at = 0;
    while (at <= raw.size()) {
        size_t nl = raw.find('\n', at);
        if (nl == std::string::npos) nl = raw.size();
        const std::string line = Trim(raw.substr(at, nl - at));
        TestStatus st;
        std::wstring name;
        bool hit = false;
        if (line.compare(0, 5, "PASS ") == 0) {
            st = TestStatus::Pass;
            name = tf::FromUtf8(Trim(line.substr(5)));
            hit = true;
        } else if (line.compare(0, 13, "COMPILE-FAIL ") == 0) {
            st = TestStatus::CompileFail;
            name = tf::FromUtf8(Trim(line.substr(13)));
            hit = true;
        } else if (line.compare(0, 10, "NORUN(run=") == 0) {
            st = TestStatus::NoRun;
            const size_t close = line.find(')');
            if (close != std::string::npos && close + 1 < line.size()) {
                name = tf::FromUtf8(Trim(line.substr(close + 1)));
                hit = true;
            }
        } else if (line.compare(0, 9, "FAIL(run=") == 0) {
            st = TestStatus::Fail;
            const size_t close = line.find(')');
            if (close != std::string::npos && close + 1 < line.size()) {
                name = tf::FromUtf8(Trim(line.substr(close + 1)));
                hit = true;
            }
        }
        if (hit) {
            if (name.empty()) {
                r.error = L"轨迹里有无名的逐测试行";
                return r;
            }
            if (seen.count(name)) {
                r.error = L"轨迹里测试名重复：" + name;
                return r;
            }
            seen[name] = st;
            r.outcomes.push_back({name, st});
        }
        if (nl == raw.size()) break;
        at = nl + 1;
    }
    if (r.outcomes.empty()) {
        r.error = L"轨迹里没有逐测试结果（无法核验，不能打分）";
        return r;
    }
    int passed = 0, notRun = 0, failed = 0, compileFail = 0;
    if (!ParseSummary(raw, &passed, &notRun, &failed, &compileFail)) {
        r.error = L"轨迹缺少汇总行（通过/无法运行/运行失败/编译失败），无法对账";
        return r;
    }
    for (const TestOutcome& o : r.outcomes) {
        if (o.status == TestStatus::Pass) ++r.score.passed;
        else if (o.status == TestStatus::NoRun) ++r.score.notRun;
        else if (o.status == TestStatus::CompileFail) ++r.score.compileFail;
        else ++r.score.failed;
    }
    // 汇总与逐项对不上 = 轨迹自相矛盾。此时不能选一个"看起来对"的口径。
    if (r.score.passed != passed || r.score.notRun != notRun ||
        r.score.failed != failed || r.score.compileFail != compileFail) {
        r.error = L"轨迹汇总与逐测试行对不上（可能被人改过），拒绝打分";
        return r;
    }
    r.score.value = static_cast<double>(r.score.passed) /
                    static_cast<double>(std::max(1, r.score.passed + r.score.failed +
                                                        r.score.compileFail));
    r.ok = true;
    return r;
}

bool BuildTrials(const Trajectory& baseline, const Trajectory& candidate,
                 std::vector<Trial>* out, std::wstring* err) {
    if (!baseline.ok || !candidate.ok) {
        if (err) *err = L"基线或候选轨迹不可信，无法配对";
        return false;
    }
    std::map<std::wstring, TestStatus> base;
    for (const TestOutcome& o : baseline.outcomes) base[o.name] = o.status;
    std::vector<Trial> trials;
    for (const TestOutcome& o : candidate.outcomes) {
        const auto it = base.find(o.name);
        if (it == base.end()) {
            Trial t;
            t.task = o.name;
            t.valid = false;
            t.reason = L"基线没有这个测试（测试集不一致）";
            trials.push_back(std::move(t));
            continue;
        }
        // 两边都无法运行 = 这一项没有证据，剔除（不是平局）。
        if (o.status == TestStatus::NoRun && it->second == TestStatus::NoRun) continue;
        // 只有一边无法运行 = 不可比，记成无效对；接受器会 fail-closed。
        if (o.status == TestStatus::NoRun || it->second == TestStatus::NoRun) {
            Trial t;
            t.task = o.name;
            t.valid = false;
            t.reason = L"只有一侧跑出了结果（无法运行）";
            trials.push_back(std::move(t));
            continue;
        }
        Trial t;
        t.task = o.name;
        t.candidate = (o.status == TestStatus::Pass) ? 1.0 : 0.0;
        t.baseline = (it->second == TestStatus::Pass) ? 1.0 : 0.0;
        trials.push_back(std::move(t));
    }
    for (const auto& kv : base) {
        bool found = false;
        for (const TestOutcome& o : candidate.outcomes)
            if (o.name == kv.first) found = true;
        if (!found) {
            Trial t;
            t.task = kv.first;
            t.valid = false;
            t.reason = L"候选没有跑这个测试（测试集不一致）";
            trials.push_back(std::move(t));
        }
    }
    if (out) *out = std::move(trials);
    return true;
}

FunnelResult RunFunnel(const Trajectory& main, const std::vector<Trajectory>& repeats,
                       const std::vector<std::wstring>& protectedChanged,
                       const std::vector<ArchivedFile>& files) {
    FunnelResult r;
    bool prev = true;
    auto add = [&](size_t i, bool pass, const std::wstring& reason) {
        StageResult s;
        s.stage = kStages[i];
        s.passed = prev && pass;
        s.reason = prev ? reason : L"前一级未通过，本级跳过";
        prev = s.passed;
        r.stages.push_back(std::move(s));
    };

    // 1. parse：轨迹必须真的解析出逐测试结果。
    add(0, main.ok, main.ok ? L"逐测试行与汇总行一致"
                            : (main.error.empty() ? L"解析失败" : main.error));

    // 2. prescan：候选不许改评估面，文件数量/体积有上限。
    //
    // protectedChanged 非空就直接不过 —— 不要求"恰好这个文件也在候选源码清单里"：
    // 被删掉的测试文件、新增的恶意测试、改过的构建脚本，本就不一定出现在
    // dev.diff 的文件清单里（例如它已经被提交，或它是目录）。
    // 改判据这件事本身 irreversible 地污染了这次评估，无须再问它是不是"候选源码"。
    unsigned long long total = 0;
    bool pathsOk = true;
    for (const ArchivedFile& f : files) {
        total += f.bytes;
        if (f.path.empty() || f.path.find(L"..") != std::wstring::npos ||
            f.path.front() == L'/' || f.path.front() == L'\\' ||
            f.path.find(L':') != std::wstring::npos)
            pathsOk = false;
    }
    const bool prescanOk =
        protectedChanged.empty() && pathsOk && files.size() <= 200 &&
        total <= 16ull * 1024 * 1024;
    add(1, prescanOk,
        prescanOk ? L"未触碰评估面，文件数与体积在上限内"
                  : L"候选改动了评估面、路径不合法，或文件数/体积超限");

    // 3. compile：编译失败数必须为 0。
    add(2, main.score.compileFail == 0,
        main.score.compileFail == 0 ? L"无编译失败"
                                    : L"有 " + std::to_wstring(main.score.compileFail) +
                                          L" 个测试编译失败");

    // 4. checks：测试失败数必须为 0。
    add(3, main.score.failed == 0,
        main.score.failed == 0 ? L"无测试失败"
                               : L"有 " + std::to_wstring(main.score.failed) +
                                     L" 个测试失败");

    // 5. stable rollout：至少两次重复，且逐测试结果与首次完全一致。
    bool stable = repeats.size() >= 2;
    for (const Trajectory& t : repeats)
        if (!t.ok || !SameOutcomes(t.outcomes, main.outcomes)) stable = false;
    add(4, stable,
        stable ? L"重复运行逐测试结果一致"
               : L"少于 2 次重复运行，或重复结果不一致");

    r.ok = prev;
    return r;
}

}  // namespace devarchive
