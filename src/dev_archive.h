// 自我迭代的接受器 + archive + validity funnel（报告 §7.1 序 7 / C2b+C1+C2+C3）。
//
// 【为什么先做接受器，再做 archive】
// 报告对标 PACE（arXiv:2606.08106）：贪心接受（"分数涨了就留"）在有真实收益时
// 30–42% 虚假提交、10–33% 有害；无收益时每次运行 13–21 次伪自修改（72–100% 虚假）。
// 没有接受器，archive 只会越滚越脏。所以判定规则必须先落地，而且必须是
// **配对比较 + betting e-process，E ≥ 1/α 才收** —— 不是单次分数比较。
//
// 【这里的证据口径】
// 判据来自本仓真实可核验的东西：dev.build / dev.test 的**原始执行轨迹**
// （tests/run_all.sh 逐行输出），不是 agent 自报的"我觉得改好了"。
// archive 自己解析原始轨迹得到逐测试结果与分数，必要时与轨迹里的汇总行对账；
// 对不上就 fail-closed，不给出一个看起来合理的分数。
#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace devarchive {

// ---- 接受器常量：不允许调用方运行时调小 ----
//
// 理由与 dev_tools 的硬超时相同：让被看管的人自己决定看管强度，就没有闸门。
// 需要调整只能改这里 —— 改动会留在 git 里、经过评估面守卫。
// kAlpha = 0.05 → 接受阈值 1/α = 20。
// λ = 0.5：下注分数。X ∈ {+1,0,-1} 时因子 1+λX ∈ {0.5,1,1.5} > 0，
// 在 H0（平均优势 ≤ 0）下 E_t = ∏(1+λX_i) 是 E_0=1 的非负上鞅，
// Ville 不等式给出 P(∃t: E_t ≥ 1/α) ≤ α —— 任意停止点都控制假接受率。
constexpr double kAlpha = 0.05;
constexpr double kLambda = 0.5;
constexpr double kThreshold = 20.0;  // 1 / kAlpha

// 一次配对试验：同一任务上分别跑候选与基线，比较两者的结果。
// valid=false 表示这一对没有可比性（超时、没跑、轨迹对不上）——
// 统计上**不能**把它当 0 分丢掉：丢弃不利证据本身就是把假绿做出来。
struct Trial {
    std::wstring task;       // 配对任务标识（测试名）
    double candidate = 0;    // 候选是否通过（1=通过，0=未通过）
    double baseline = 0;     // 基线是否通过
    bool valid = true;       // 这一对是否可用
    std::wstring reason;     // 不可用的原因
};

struct DecideResult {
    bool ok = false;
    bool accepted = false;
    std::wstring decision;   // "accept" / "reject" / "insufficient"
    double eProcess = 1.0;   // 当前 E_t（从 1 开始）
    double threshold = kThreshold;
    int trials = 0;          // 有效配对数
    int wins = 0;
    int ties = 0;
    int losses = 0;
    int invalid = 0;         // 不可用配对数（>0 时一律不接受）
    std::wstring reason;     // 人可读判定说明
    std::vector<double> trace;  // 每一步之后的 E 值（原始序列，供审计）
};

// 纯计算：给定配对试验序列，给出接受器判定。不读盘、不改状态、不摘要。
// 注意判定**不**用"总分涨了"：只看逐对符号的乘积过程是否越过 1/α。
DecideResult Decide(const std::vector<Trial>& trials);

// JSON 版（供命令层）：trials 是数组，每项 {task, candidate, baseline}。
// 解析失败返回 ok=false，并给出原因；不静默把坏数据当成平局。
DecideResult Decide(const json::Value& trialsJson);

// ---- 原始执行轨迹 → 逐测试结果 + 分数 ----
enum class TestStatus { Pass, Fail, CompileFail, NoRun };

struct TestOutcome {
    std::wstring name;
    TestStatus status = TestStatus::Fail;
};

struct Score {
    int passed = 0;
    int failed = 0;
    int compileFail = 0;
    int notRun = 0;
    double value = 0;  // passed / max(1, passed+failed+compileFail)；notRun 不算成功也不算失败
};

struct Trajectory {
    bool ok = false;
    std::wstring error;
    std::string raw;                       // UTF-8 原文（逐字保存，不摘要）
    std::vector<TestOutcome> outcomes;
    Score score;
};

// 解析 tests/run_all.sh 的原文。返回 ok=false 表示"这份轨迹不可信"
// （没有逐测试行 / 名字重复 / 逐项计数与汇总行对不上）。
Trajectory ParseTrajectory(const std::string& raw);

// 用同一套测试名把基线与候选配对。两个轨迹的测试名集合必须一致；
// 不一致（新增/漏跑测试）时返回 false —— 配对不成立，不接受。
bool BuildTrials(const Trajectory& baseline, const Trajectory& candidate,
                 std::vector<Trial>* out, std::wstring* err);

// ---- validity funnel（parse → prescan → compile → checks → stable rollout）----
struct StageResult {
    std::wstring stage;      // parse / prescan / compile / checks / stable
    bool passed = false;
    std::wstring reason;
};

struct FunnelResult {
    bool ok = false;         // 五级全过
    std::vector<StageResult> stages;  // 始终 5 项，逐级给出；失败后后续标 skipped
};

struct ArchivedFile {
    std::wstring path;       // 仓库内相对路径
    std::wstring sha256;
    unsigned long long bytes = 0;
};

// 逐级过滤。protectedChanged 是评估面被改动的路径清单（来自 evalsurface::Check()），
// 命中任何一个 → prescan 不过（改判据就是作弊，不能进 archive）。
// repeats 是"稳定上线"阶段的重复运行；至少 2 次且逐测试结果与首次完全一致才算稳定。
FunnelResult RunFunnel(const Trajectory& main, const std::vector<Trajectory>& repeats,
                       const std::vector<std::wstring>& protectedChanged,
                       const std::vector<ArchivedFile>& files);

// ---- archive 落盘 ----
// 目录布局：<root>/<id>/{manifest.json, files/<相对路径>, trajectory/raw.txt,
//                        trajectory/repeat-N.txt}
// root 为空时用 <exe 目录>\archive（与 snapshots/ 一样在 .gitignore 里）。
struct RecordOptions {
    std::wstring root;                    // 空 = 默认
    std::wstring id;                      // 空 = 自动生成
    std::wstring parentId;                // lineage：上一份被接受的候选
    std::wstring baselineId;              // 配对基线（有它才能算接受器）
    std::wstring note;
    std::string rawTrajectory;            // **原始**轨迹，逐字保存
    std::vector<std::string> repeatTrajectories;
    std::vector<std::wstring> protectedChanged;
};

struct RecordResult {
    bool ok = false;
    std::wstring error;
    std::wstring id;
    std::wstring dir;
    std::wstring baselineId;
    Score score;
    DecideResult decision;
    FunnelResult funnel;
    int files = 0;
};

// 落盘一份候选：源码副本 + 分数 + 原始执行轨迹 + funnel + 接受器判定。
// **不做预摘要**：轨迹原文单独存文件，manifest 只记它的字节数和 SHA-256。
// 任何一步失败都不留下半份记录（先建临时目录，成功后再改名）。
RecordResult Record(const RecordOptions& opt);

struct Entry {
    std::wstring id;
    std::wstring createdAt;
    std::wstring parentId;
    std::wstring baselineId;
    Score score;
    bool accepted = false;
    double eProcess = 1.0;
    bool funnelOk = false;
    int files = 0;
};

// 列出 archive 里的候选（新的在前）。读不到某一项就跳过并把它记入 skipped，
// 不因为一条坏记录就让整个列表看起来是空的。
struct ListResult {
    std::vector<Entry> entries;
    std::vector<std::wstring> skipped;
};
ListResult List(const std::wstring& root);

// 读回某一份候选的原始轨迹（按名字：raw / repeat-1 ...）。找不到返回 false。
bool ReadTrajectory(const std::wstring& root, const std::wstring& id,
                    const std::wstring& name, std::string* out);

std::wstring DefaultRoot();

}  // namespace devarchive
