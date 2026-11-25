// 接受器 + archive + funnel 的行为级单测。
//
// 【最关键的一条是蒙特卡洛】在 H0（候选与基线没有真实差异）下反复跑接受器，
// 统计"越过 E ≥ 1/α"的比例。若判定只是"分数涨了就留"，这个比例会接近 100%；
// e-process 必须让它 ≤ α。这是"不是假绿"的可执行证据，不是注释里的保证。
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "dev_archive.h"
#include "dev_proc.h"
#include "text_file.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Expect(bool c, const char* what) {
    if (c) ++g_pass;
    else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

// 确定性 LCG：测试必须可复现，不能靠随机种子碰运气。
struct Rng {
    unsigned long long s = 0x123456789abcdefULL;
    unsigned Next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned>(s >> 33);
    }
};

std::wstring TempRoot() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    const std::wstring base = n ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"eb_archive_" + std::to_wstring(GetCurrentProcessId());
}

bool Git(const std::wstring& cwd, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> full = {L"-c", L"core.quotepath=false"};
    for (const auto& a : args) full.push_back(a);
    int code = 0;
    std::wstring out, err;
    bool overflow = false;
    if (!devproc::RunCapture(L"git", full, cwd, &code, &out, &err, &overflow)) return false;
    return code == 0;
}

void RemoveTree(const std::wstring& root) {
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\"";
    _wsystem(cmd.c_str());
}

std::string Raw(const std::string& passNames, int passed, int failed, int norun, int cfail) {
    return passNames + "通过 " + std::to_string(passed) + " / 无法运行 " +
           std::to_string(norun) + " / 运行失败 " + std::to_string(failed) +
           " / 编译失败 " + std::to_string(cfail) + "（共 " +
           std::to_string(passed + failed + norun + cfail) + " 个测试）\n";
}

}  // namespace

int main() {
    std::printf("接受器 + archive + funnel\n");

    // ---- 1. 接受器的基本性质 ----
    {
        std::vector<devarchive::Trial> allWin(8);
        for (auto& t : allWin) { t.task = L"t"; t.candidate = 1; t.baseline = 0; }
        const auto d = devarchive::Decide(allWin);
        Expect(d.ok && d.accepted, "8 连胜：E=1.5^8=25.6 ≥ 20，接受");
        Expect(d.trials == 8 && d.wins == 8 && d.trace.size() == 9,
               "trace 含初始 E_0=1 与每步结果");

        std::vector<devarchive::Trial> mixed(8);
        for (int i = 0; i < 8; ++i) {
            mixed[i].task = L"t";
            mixed[i].candidate = (i % 2 == 0) ? 1 : 0;
            mixed[i].baseline = (i % 2 == 0) ? 0 : 1;
        }
        const auto m = devarchive::Decide(mixed);
        Expect(!m.accepted && m.wins == 4 && m.losses == 4,
               "输赢交替：E 跌到 0.5^4·1.5^4 < 1，拒绝（分数有涨有跌不算改进）");

        const auto empty = devarchive::Decide(std::vector<devarchive::Trial>{});
        Expect(!empty.accepted && empty.decision == L"insufficient",
               "空集不是证据");

        std::vector<devarchive::Trial> bad(8);
        for (auto& t : bad) { t.candidate = 1; t.baseline = 0; }
        bad[3].valid = false;
        bad[3].reason = L"超时";
        const auto b = devarchive::Decide(bad);
        Expect(!b.accepted && b.invalid == 1,
               "有不可用配对就 fail-closed（不许把没跑的当 0 丢掉）");
    }

    // ---- 2. 蒙特卡洛：H0 下假接受率 ≤ α ----
    {
        constexpr int kRuns = 5000;
        constexpr int kMaxTrials = 40;
        Rng rng;
        int falseAccept = 0;
        for (int run = 0; run < kRuns; ++run) {
            std::vector<devarchive::Trial> ts;
            ts.reserve(kMaxTrials);
            for (int i = 0; i < kMaxTrials; ++i) {
                devarchive::Trial t;
                t.task = L"t" + std::to_wstring(i);
                // H0：候选与基线同分布，输赢各半 → X 的均值为 0。
                const bool win = (rng.Next() % 2) == 0;
                t.candidate = win ? 1 : 0;
                t.baseline = win ? 0 : 1;
                ts.push_back(t);
            }
            if (devarchive::Decide(ts).accepted) ++falseAccept;
        }
        const double rate = static_cast<double>(falseAccept) / kRuns;
        std::printf("  H0 假接受率：%d/%d = %.4f（α=%.2f）\n", falseAccept, kRuns, rate,
                    devarchive::kAlpha);
        Expect(rate <= devarchive::kAlpha,
               "H0 下 P(接受) ≤ α —— 任意停止点控制虚假提交");

        // 反向证据：H1（真实优势 80% 胜率）下应当能收，否则接受器只是"永远拒绝"。
        Rng rng2;
        int trueAccept = 0;
        for (int run = 0; run < 200; ++run) {
            std::vector<devarchive::Trial> ts;
            for (int i = 0; i < kMaxTrials; ++i) {
                devarchive::Trial t;
                const bool win = (rng2.Next() % 10) < 8;
                t.candidate = win ? 1 : 0;
                t.baseline = win ? 0 : 1;
                ts.push_back(t);
            }
            if (devarchive::Decide(ts).accepted) ++trueAccept;
        }
        std::printf("  H1 检出率：%d/200 = %.4f\n", trueAccept, trueAccept / 200.0);
        Expect(trueAccept >= 190, "H1（80% 胜率）几乎必收 —— 接受器有功效");
    }

    // ---- 3. 轨迹解析：原始行 → 分数，篡改要对不出来 ----
    {
        const std::string good = Raw("PASS a_test\nFAIL(run=1) b_test\n", 1, 1, 0, 0);
        const auto t = devarchive::ParseTrajectory(good);
        Expect(t.ok && t.score.passed == 1 && t.score.failed == 1,
               "逐测试行与汇总行一致：分数如实解析");

        const std::string tampered = Raw("PASS a_test\nPASS b_test\n", 1, 1, 0, 0);
        Expect(!devarchive::ParseTrajectory(tampered).ok,
               "把 FAIL 改成 PASS 但汇总没改 → 对不上，拒绝打分");

        Expect(!devarchive::ParseTrajectory("通过 5 / 无法运行 0 / 运行失败 0 / 编译失败 0\n").ok,
               "只有汇总、没有逐测试行 → 不可信");

        const std::string dup = Raw("PASS a_test\nPASS a_test\n", 2, 0, 0, 0);
        Expect(!devarchive::ParseTrajectory(dup).ok, "重复测试名 → 不可信");
    }

    // ---- 4. 配对：两边必须同一套测试名 ----
    {
        const auto base = devarchive::ParseTrajectory(Raw("FAIL(run=1) a\nPASS b\n", 1, 1, 0, 0));
        const auto cand = devarchive::ParseTrajectory(Raw("PASS a\nFAIL(run=1) b\n", 1, 1, 0, 0));
        std::vector<devarchive::Trial> trials;
        std::wstring err;
        Expect(devarchive::BuildTrials(base, cand, &trials, &err) && trials.size() == 2,
               "同测试集配对成功");
        const auto d = devarchive::Decide(trials);
        Expect(d.wins == 1 && d.losses == 1 && !d.accepted,
               "一对一输赢：无净优势，拒绝");

        // 测试集不一致：不直接判配对函数失败，而是产生"无效配对"，
        // 由接受器 fail-closed —— 这样漏跑测试不会变成"少算一票平局"。
        const auto fewer = devarchive::ParseTrajectory(Raw("PASS a\n", 1, 0, 0, 0));
        Expect(devarchive::BuildTrials(base, fewer, &trials, &err) && !trials.empty(),
               "候选漏跑测试 → 记录成无效配对");
        const auto incomplete = devarchive::Decide(trials);
        Expect(!incomplete.accepted && incomplete.invalid > 0,
               "无效配对 → 接受器拒绝（不许悄悄丢掉漏跑的测试）");

        // 两边都无法运行的项应被剔除，不是当成平局更不是当成失败。
        const auto nb = devarchive::ParseTrajectory(
            Raw("NORUN(run=77) a\nNORUN(run=77) b\n", 0, 0, 2, 0));
        const auto nc = devarchive::ParseTrajectory(
            Raw("NORUN(run=77) a\nNORUN(run=77) b\n", 0, 0, 2, 0));
        Expect(devarchive::BuildTrials(nb, nc, &trials, &err) && trials.empty(),
               "两边都无法运行 → 没有可比证据，剔除");
    }

    // ---- 5. funnel 逐级过滤 ----
    {
        const auto main = devarchive::ParseTrajectory(Raw("PASS a\n", 1, 0, 0, 0));
        const std::vector<devarchive::Trajectory> reps = {main, main};
        const auto full = devarchive::RunFunnel(main, reps, {}, {});
        Expect(full.ok && full.stages.size() == 5, "五级全过");

        const auto badRep = devarchive::ParseTrajectory(Raw("FAIL(run=1) a\n", 0, 1, 0, 0));
        const auto unstable = devarchive::RunFunnel(main, {main, badRep}, {}, {});
        Expect(!unstable.ok && !unstable.stages[4].passed,
               "重复运行不一致 → stable rollout 不过");

        const auto tampered = devarchive::RunFunnel(
            main, reps, {L"build_fast.py"}, {});
        Expect(!tampered.ok && !tampered.stages[1].passed,
               "候选改了评估面 → prescan 不过");

        const auto noRepeats = devarchive::RunFunnel(main, {}, {}, {});
        Expect(!noRepeats.ok && !noRepeats.stages[4].passed,
               "没有重复运行 → stable rollout 不过");

        const auto compileFail = devarchive::ParseTrajectory(
            Raw("COMPILE-FAIL a\n", 0, 0, 0, 1));
        const auto cf = devarchive::RunFunnel(compileFail, {}, {}, {});
        Expect(!cf.stages[2].passed && !cf.stages[3].passed && !cf.stages[4].passed,
               "compile 不过时后续级别全跳过（逐级过滤）");
    }

    // ---- 6. archive 落盘往返：源码 + 分数 + 原始轨迹 ----
    {
        const std::wstring root = TempRoot();
        RemoveTree(root);
        tf::EnsureDir(root);
        Git(root, {L"init", L"-q", L"."});
        Git(root, {L"config", L"user.email", L"t@example.com"});
        Git(root, {L"config", L"user.name", L"t"});
        Git(root, {L"config", L"core.autocrlf", L"false"});
        tf::WriteText(root + L"\\seed.txt", L"initial\n");
        Git(root, {L"add", L"seed.txt"});
        Git(root, {L"commit", L"-q", L"-m", L"seed"});
        tf::WriteText(root + L"\\candidate.cpp", L"// 候选源码\nint main(){}\n");

        const std::string raw = Raw("PASS a_test\nPASS b_test\n", 2, 0, 0, 0);
        devarchive::RecordOptions opt;
        opt.root = root + L"\\archive";
        opt.id = L"cand-test";
        opt.note = L"行为级单测";
        opt.rawTrajectory = raw;
        opt.repeatTrajectories = {raw, raw};
        const auto rec = devarchive::Record(opt);
        Expect(rec.ok, "落盘成功");
        Expect(rec.score.passed == 2 && rec.score.value == 1.0, "分数来自轨迹而非自报");
        Expect(rec.decision.decision == L"insufficient",
               "没有 baselineId 时明说证据不足，不伪造接受");
        Expect(rec.funnel.ok, "两次一致重复 → funnel 通过");

        std::string back;
        Expect(devarchive::ReadTrajectory(opt.root, L"cand-test", L"raw", &back) &&
                   back == raw,
               "原始轨迹逐字往返（含中文汇总行）");

        const auto list = devarchive::List(opt.root);
        Expect(list.entries.size() == 1 && list.entries[0].id == L"cand-test" &&
                   list.entries[0].funnelOk && list.entries[0].files >= 1,
               "list 读回候选：id / funnel / 源码文件数一致");

        // 第二次记录把第一次当基线：所有测试都通过 → 平局 → 不接受。
        devarchive::RecordOptions opt2 = opt;
        opt2.id = L"cand-second";
        opt2.baselineId = L"cand-test";
        const auto rec2 = devarchive::Record(opt2);
        Expect(rec2.ok && !rec2.decision.accepted && rec2.decision.trials == 2 &&
                   rec2.decision.ties == 2,
               "与基线逐项平局 → e-process 停在 1，拒绝接受");

        RemoveTree(root);
    }

    std::printf("通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
