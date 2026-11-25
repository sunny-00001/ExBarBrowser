// 失败分类自测（P0-4 ③：`blocked` 不再一锅端）。
//
// 【为什么这一层必须测】"被拦住"的四类码是给机器分支用的：
// 调用方靠它决定下一步是"换目标"、"稍后重试"、"问人"还是"换一条路"。
// 一旦退化成一锅端，agent 只能猜 —— 而它猜错的方式通常是
// "一直重试到超时"或"直接放弃"，两种都会变成静默的错误行为。
//
// 这里钉三件事：
//   1. FailKind → errorCode 的映射是**唯一真源**（改了枚举忘了改码表会红）；
//   2. 类别能从命令调用点一路透传到总线（不是只存在某个局部变量里）；
//   3. **被取消的作业不能报成功** —— 以前 CollectResult 不判 cancelled，
//      被取消的作业会走"成功"分支，把空结果当成"跑完了"报出去
//      （"查重没查到重复"其实可能是"用户中途叫停了"）。
#include "common.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <windows.h>

#include "app_commands_jobs.h"
#include "commands.h"
#include "config.h"
#include "job.h"
#include "json.h"
#include "scan_ctl.h"
#include "text_file.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool Is(const wchar_t* code, const wchar_t* want) {
    return std::wstring(code ? code : L"") == std::wstring(want);
}

// 审计文件不能写到用户的 data\ 里去 —— 用临时目录。
std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return n ? std::wstring(buf, n) : std::wstring(L".");
}

std::wstring ReadAll(const std::wstring& path) {
    std::wstring out;
    if (!tf::ReadText(path, &out)) return out;
    return out;
}

}  // namespace

int main() {
    // 审计落到独立临时目录（不碰用户的 data\）。
    const std::wstring dir = tf::Join(TempDir(), L"ebb_fail_kind_test");
    CreateDirectoryW(dir.c_str(), nullptr);
    Config::Get().SetProfileDir(dir);
    const std::wstring logs = tf::Join(dir, L"logs");
    tf::EnsureDir(logs);
    const std::wstring jsonlPath = tf::Join(logs, L"audit.jsonl");
    DeleteFileW(jsonlPath.c_str());
    // ---------------------------------------------------------------
    std::printf("FailKind → errorCode 的映射\n");
    {
        Check(Is(FailKindCode(FailKind::PreconditionUnmet), L"precondition_unmet"),
              "PreconditionUnmet → precondition_unmet");
        Check(Is(FailKindCode(FailKind::LockConflict), L"lock_conflict"),
              "LockConflict → lock_conflict");
        Check(Is(FailKindCode(FailKind::CanceledByUser), L"canceled_by_user"),
              "CanceledByUser → canceled_by_user");
        Check(Is(FailKindCode(FailKind::NotSupported), L"not_supported"),
              "NotSupported → not_supported");
        Check(Is(FailKindCode(FailKind::TimedOut), L"timed_out"),
              "TimedOut → timed_out");
        Check(Is(FailKindCode(FailKind::None), L""),
              "None 不是一种错误（空码）");
    }

    // ---------------------------------------------------------------
    std::printf("\n类别能从调用点透传到总线\n");
    {
        CommandBus::Get().Register(CmdId::TabNew, [](const Args&) {
            CommandBus::Get().NoteFailure(L"剪贴板被别的程序占着", FailKind::LockConflict);
            return false;
        });
        const bool ok = CommandBus::Get().Invoke(CmdId::TabNew, Args::Empty(),
                                                 Actor{ActorKind::Ai, L"test"});
        Check(!ok, "handler 报失败 -> 调用失败");
        Check(CommandBus::Get().lastFailKind() == FailKind::LockConflict,
              "锁冲突的类别透传到总线");
        Check(CommandBus::Get().lastBlockReason() == L"剪贴板被别的程序占着",
              "人可读的原因也透传到总线");
    }

    std::printf("\n审计里的 errorCode 是机器码，人读原因另立字段\n");
    {
        // 【为什么这条重要】errorCode 是给机器分支用的。
        // 以前 Dispatch 把**中文原因**塞进了 errorCode —— 机器字段被污染，
        // 消费方只能靠匹配文案分派（文案一改就断）。
        // 而且 AI 通道返回的码与审计记的码必须**同一个来源**，
        // 否则同一件事有两个真相。
        const std::wstring jsonl = ReadAll(jsonlPath);
        Check(jsonl.find(L"\"errorCode\":\"lock_conflict\"") != std::wstring::npos,
              "审计记的是机器码 lock_conflict");
        Check(jsonl.find(L"\"reason\":\"剪贴板被别的程序占着\"") != std::wstring::npos,
              "人可读的原因记在 reason 字段");
        Check(jsonl.find(L"\"errorCode\":\"剪贴板") == std::wstring::npos,
              "机器码字段里没有中文");
    }

    std::printf("\n不给类别时默认 precondition_unmet\n");
    {
        CommandBus::Get().Register(CmdId::GoHome, [](const Args&) {
            CommandBus::Get().NoteFailure(L"目标不在当前视图：X");
            return false;
        });
        CommandBus::Get().Invoke(CmdId::GoHome, Args::Empty(),
                                 Actor{ActorKind::Ai, L"test"});
        Check(CommandBus::Get().lastFailKind() == FailKind::PreconditionUnmet,
              "默认类别是 precondition_unmet");
    }

    std::printf("\n前置不满足由总线直接归类\n");
    {
        // NavBack 要求有历史；探针给"无历史" -> 派发层就该拒绝，
        // 而且类别是 precondition_unmet（它本来就是这个定义）。
        SetStateProbe([] {
            CommandState s;
            s.tabCount = 1;
            s.historyBack = 0;
            s.historyForward = 0;
            return s;
        });
        CommandBus::Get().Register(CmdId::NavBack, [](const Args&) { return true; });
        const bool ok = CommandBus::Get().Invoke(CmdId::NavBack, Args::Empty(),
                                                 Actor{ActorKind::Ai, L"test"});
        Check(!ok, "无历史时拒绝执行");
        Check(CommandBus::Get().lastFailKind() == FailKind::PreconditionUnmet,
              "类别是 precondition_unmet");
        Check(!CommandBus::Get().lastBlockReason().empty(), "给出了人可读的原因");
    }

    // ---------------------------------------------------------------
    std::printf("\n被取消的作业不能报成功\n");
    {
        job::Service& svc = job::Service::Get();
        const std::wstring id = svc.Start(
            L"test", L"C:\\tmp",
            [](scanctl::Control& ctl, json::Value* out, std::wstring*) {
                // 一直等到被取消为止（最多 3 秒，防止测试挂住）。
                for (int i = 0; i < 300 && !ctl.Cancelled(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                out->Set(L"partial", json::Value::MakeBool(true));
                return true;    // 正常收工，但它已被取消
            });
        Check(!id.empty(), "作业起来了");
        svc.Cancel(id);
        svc.WaitFinished(id, 5000);

        json::Value out;
        std::wstring err;
        FailKind kind = FailKind::None;
        const bool ok = appjobs::CollectResult(id, false, &out, &err, &kind);
        Check(!ok, "被取消的作业**不算成功**");
        Check(kind == FailKind::CanceledByUser, "类别是 canceled_by_user");
        Check(!err.empty(), "给了人可读的原因");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
