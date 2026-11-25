// 长任务（作业）域的命令层共用入口。
//
// 【这一域是什么】tool.dupfinder / tool.hash / tool.compare / tool.diskusage /
// tool.topn 五条命令的活可能跑很久。它们过去同步跑在 UI 线程上：
// 一个几万文件的目录能把界面冻住十几秒，用户连"取消"都按不了
// （UI 线程被占着，连定时器都不转）。
// 现在重活搬到工作线程（src/job.cpp），UI 线程只负责准备参数与取结果，
// 中间由这里的 RunTask 把"起作业 / 等作业 / 收结果"统一起来。
//
// 【为什么共用入口要单列一个头】五条命令的并发约定完全一样
// （工作线程 + 进度 + 取消 + async）。约定散在五个文件里，
// 迟早有一处写成"忘了查取消位"。收在一处，改一处就够。
//
// 【人工与机器是两条路，但只有一条实现】
//   人工：弹进度窗口（带"取消"按钮），用户随时能停；结束后弹只读报告。
//   机器：不弹窗（没人盯着屏幕），但照样泵消息，界面不会僵死；
//         10 秒管道超时后仍要能继续 —— 所以另有 async 形态（立刻返回 jobId）。
#pragma once
#include <string>

#include "commands.h"   // FailKind（失败分类，见 commands.h）
#include "common.h"
#include "job.h"
#include "json.h"
#include "theme.h"

namespace appjobs {

// 一次长任务的参数。
struct TaskSpec {
    std::wstring kind;    // 作业类别（hash / dup / compare / usage / topn）
    std::wstring title;   // 进度窗口标题（人看）
    std::wstring target;  // 人可读描述（一般是路径）
    bool async = false;   // true = 立刻返回 jobId，不等它跑完
    bool human = false;   // true = 弹进度窗口
    unsigned waitMs = 30000;  // 同步路径最多等多久（毫秒）
};

// 时间预算的统一夹取（毫秒）。
// 太小必然没结果，太大会让人以为程序卡死 —— 上下界与旧实现一致，
// 免得同一份脚本在不同版本上表现突变。
unsigned long long ClampBudget(int ms);

// 解析一次长任务的时间预算。返回 0 = 不限时（见 scanctl::kUnlimitedBudget）。
//
// 【三个默认值不是同一件事，各有各的理由】
//   · 调用方显式给了 budgetMs → 夹到 100..30000。不擅自改契约：
//     同一个脚本在新旧版本上跑出一样的结果，比"更聪明"重要。
//   · 没给，且是**机器同步路径** → 1500ms（与旧实现一致）。
//     机器调用不能久等：AI 管道的监听线程 10 秒就超时，作业还占着 UI 线程的话，
//     调用方既拿不到结果，也没法叫停。
//   · 没给，且是**机器异步路径** → 不限时。
//     异步的用处就是"这活很久，别等我"。给它 1.5 秒的默认预算，作业会在调用方
//     还没来得及查状态时就被判超时 —— 异步就退化成了"更麻烦的同步"。
//   · 没给，且是**人工路径**（有进度窗口和取消按钮）→ 不限时。
//     旧实现也是 1500ms，但那时活跑在 UI 线程上，1.5 秒卡顿是能接受的代价；
//     现在活跑在工作线程上、还带进度和取消按钮，半路掐断反而成了没必要的打扰。
unsigned long long ResolveBudget(bool hasBudget, int budgetMs, bool async, bool human);

// 起一个长任务并按需等它跑完。返回作业 id（空串 = 启动失败）。
//
// 【同步 vs 异步】
//   同步（async=false，默认）：在 UI 线程上等作业跑完，返回完整结果。
//   异步（async=true）：立刻返回 {jobId, state:"running"}，
//     调用方用 tool.job.get 查、tool.job.cancel 取消。
//
// 【为什么异步是必须的】AI 通道的管道监听线程只有 10 秒超时。
// 同步扫一个大目录必然超时，而超时之后命令仍在 UI 线程上跑 ——
// 调用方拿不到结果，也没法叫停。异步把"跑多久"与"等多久"拆开。
std::wstring RunTask(HWND owner, const Theme& theme, const TaskSpec& spec, job::Body body);

// 把作业的最终状态搬进命令结果。
// 返回 false = 作业没跑完（超时）或失败（err 有人可读原因）。
//
// 【kind 是干什么的】失败原因不止一种，"下一步该做什么"也就不同：
//   · 被用户取消 → 停下来问人，重试只是骚扰
//   · 等时限到了 → **去查 tool.job.get**（作业还在后台跑），不是重试
//   · 真失败     → 看 err 原文
// 所以这里把类别一并带出去，由调用点转成 NoteFailure 的 kind ——
// 调用方拿到的是能指导下一步的机器码，而不是一句要人去猜的中文。
//
// 【取消为什么单列】作业被取消时 job::Status 里 done=true、cancelled=true、
// failed=false —— 它**没有出错**，只是没做完。以前这里只判 failed，
// 于是被取消的作业会走"成功"分支，把一个空结果当成完成的结果报出去
// （"查重没查到重复"其实可能是"用户中途叫停了"）。
bool CollectResult(const std::wstring& jobId, bool async, json::Value* result,
                   std::wstring* err, FailKind* kind = nullptr);

}  // namespace appjobs
