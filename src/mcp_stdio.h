// MCP over stdio：把本进程的标准输入输出接到命名管道上。
//
// 【为什么需要它】AI 通道原本只有命名管道一种入口。命名管道好用，但**不是 MCP
// 客户端认识的入口**：Claude Desktop / VS Code / Cursor 这类客户端只会用
// stdio（子进程 + 换行分隔的 JSON-RPC）或 HTTP 去连 MCP 服务器。
// 业界现状（2026-11 调研）：
//   · Eagle 随附本地 HTTP MCP 服务器（127.0.0.1:41596/mcp）；
//   · 微软给资源管理器做了官方 MCP 连接器；
//   · Windows ODR 把 `odr mcp add --manifest` 做成了系统级标准。
// 没有 stdio 入口 = 别的 MCP 客户端要接本程序就得先自己写一个转发器。
//
// 【为什么是"桥接"而不是"再实现一遍协议"】命令 handler 必须在 UI 线程跑
// （它们碰 shell 视图与选中集），命名管道那侧已经有"排队 → UI 线程执行 →
// 等结果（带 10s 超时）"的机制。本模块只做**字节搬运**：
//   stdin 一行 → 注入令牌 → 管道 → 管道回一行 → stdout
// 协议、鉴权、错误码、自断言、事件流全走原来那条路，
// 于是 stdio 与管道**不可能**出现两套行为。
//
// 【零依赖】只用 Win32（管道 + 文件句柄），不引入任何第三方库（AGENTS.md §1.2）。
#pragma once

#include <windows.h>

namespace mcpstdio {

// 命令行里是否带了 --mcp-stdio。
// 结果只算一次（命令行一辈子不变），可以反复问。
bool Requested();

// 启动桥接线程。
//   mainWnd —— 主窗口句柄。stdin 收到 EOF（客户端断开）时用它请程序退出。
// 返回 false = 没启动（AI 通道没起来 / 管道连不上）；原因已记日志。
bool Start(HWND mainWnd);

// 停止桥接线程并断开连接。进程退出前调用。
// 可重入：App::OnDestroy 与 App::Run 收尾各调一次，谁先到都行。
// 内部用 CancelSynchronousIo 唤醒阻塞在 stdin 上的读；
// 万一线程赖着不走，会 detach 而不是 join —— join 会把 UI 线程卡死，
// 而 detach 之后 std::thread 的析构也不会再 std::terminate()。
void Stop();

}  // namespace mcpstdio
