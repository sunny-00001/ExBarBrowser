// AI 服务的子进程管理：启动 node service/server.js，退出时确保它一起走。
//
// 【为什么由本进程拉起服务，而不是让服务自己找窗口】
//   服务原本要"猜"该连哪个 ExBarBrowser 实例：管道名内嵌 hwnd+pid，
//   data 下还有几十个 profile 目录各有一套发现文件。每次重启都变，
//   实测出现过 180 个候选、绝大多数是死实例的情况。
//
//   本进程拉起服务时，**它自己就是那个实例** —— 管道名与令牌在进程内已知，
//   直接通过环境变量传下去，服务不需要任何发现逻辑。这也是"固定端口"诉求的
//   真正解法：不是把管道名固定（不可能），而是让唯一知道它的人来传。
//
// 【生命周期】三重保证，任一失效还有兜底：
//   1. Job Object + JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE —— 本进程无论怎么死
//      （正常退出 / 崩溃 / 被任务管理器结束），系统都会连带结束子进程。
//      这是唯一能覆盖"本进程被强杀"的机制，所以必须用。
//   2. 显式 Stop()：正常关闭时先礼后兵（先等一会，再强杀）。
//   3. 子进程自己也监听父进程 pid（见 server.js 的 --parent-pid）。
//
// 【端口】不用固定 8792。并行开发时多个实例会抢同一个端口，
//   第二个必然 EADDRINUSE 退出。这里让系统分配空闲端口（bind 0 后读回），
//   再通过 AGENTCHAT_PORT 传给服务 —— 多开时互不冲突。
#pragma once

#include <windows.h>

#include <string>

namespace aisvc {

// 服务状态快照（给日志与诊断用）。
struct Status {
    bool running = false;
    DWORD pid = 0;
    unsigned port = 0;
    std::wstring reason;     // 失败原因（running=false 时有效）
};

// 启动服务。成功返回 true，端口写入 outPort。
//   scriptPath  —— service/server.js 的绝对路径
//   pipeName    —— 本实例的 AI 管道名（原样传下去，服务不再去发现）
//   token       —— 对应的令牌
//   exeDir      —— 用作服务的工作目录（它要读 ui/ 与 service/ 相对路径）
bool Start(const std::wstring& scriptPath, const std::wstring& exeDir,
           const std::wstring& pipeName, const std::wstring& token,
           unsigned* outPort, std::wstring* reason);

// 停止（先发通知让它主动退、短暂等待，再强杀兜底）。幂等。
void Stop();

// 用上次 Start 保存的参数重启。子进程意外退出或 AI 通道热切换后必须走这里，
// 否则 node 仍持有旧 pipe/token，工具调用会一直失败。
bool Restart(unsigned* outPort, std::wstring* reason);

// 本进程生成、只传给子进程与面板的本地 HTTP token。WebView/浏览器页面必须用
// X-ExBar-Token 带上它；跨源页面拿不到，因而不能驱动 /api/*。
const std::wstring& HttpToken();

// 当前状态。
Status Current();

// 服务是否已经能应答（HTTP 端口可连）。用于面板导航前的一次等待。
bool WaitReady(int timeoutMs);

// 本进程是否以"嵌入式"方式运行（有 node 服务）。面板据此决定是加载本地
// Web UI 还是显示"未启动"占位。
bool Enabled();

}  // namespace aisvc