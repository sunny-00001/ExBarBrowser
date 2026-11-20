// AI 对外通道：命名管道，JSON 进 / JSON 出。
//
// 为什么是命名管道而不是 HTTP/MCP：
//   · 零新依赖（Windows 自带），不需要引入 socket 库或 WebView；
//   · 本地进程间通行的既有做法（mod 的 helper 也走管道）；
//   · 天然按用户会话隔离（管道名带 Local\ 前缀），不需要额外鉴权层。
// MCP 客户端可在此之上做薄封装（tools/list 与 tools/call 的形状已经在 ai_api 里
// 对上 MCP 规范），不需要在 A 侧塞 JSON-RPC 状态机。
//
// 安全口径（fail-closed）：
//   · 只监听当前用户会话（Local\ 前缀 = 当前登录会话）。
//   · 默认不监听（需 settings.json 的 aiEnabled=true 显式开启）。
//   · 每个连接先发 HELLO，校验令牌；令牌每次启动随机生成，写在
//     %LOCALAPPDATA%\ExBar\ai-token.txt（仅当前用户可读）。
//     这不是加密，只是"别的进程不能随手乱调"的护栏。
#pragma once
#include <string>

#include "json.h"

namespace ai {

// 协议：一行一个请求（UTF-8 JSON），一行一个响应。
// 请求形状：
//   {"token":"...","method":"tools/list"}
//   {"token":"...","method":"tools/call","name":"NavBack","args":{},"approve":false}
//   {"token":"...","method":"state"}
// 响应形状：
//   tools/list → {"ok":true,"tools":[...]}
//   tools/call → {"ok":bool,"command":..,"risk":..,"error":..}
//   state      → {"ok":true,"activePath":..,"tabs":[..],..}

// 启动监听线程（幂等）。返回 false = 管道建不起来（已记日志）。
// 监听线程与 UI 线程解耦：只把请求 PostMessage 到主窗口，由主线程执行命令，
// 避免在管道线程里碰 Shell/UI（架构规则 §4 原则 8）。
bool StartServer(void (*postToUi)(const std::wstring& requestLine));

// 停止监听并清理管道。
void StopServer();

// 是否正在监听。供诊断与单测判断"服务真的起来了/真的停了"。
bool IsRunning();

// UI 线程调用：处理所有挂起的请求（命令 handler 只能在 UI 线程跑）。
// 返回本次处理了几个；0 = 没活干。由 App 的定时器驱动即可，不需要更复杂的唤醒。
int PumpRequests();

// 令牌文件路径（启动时写入；客户端读它来通过校验）。
std::wstring TokenPath();

// 处理一行请求，返回一行响应（纯函数，可在 UI 线程直接调；便于单测）。
// 不依赖任何窗口 —— 命令本身经 CommandBus，状态经 ai::GetState()。
//
// 支持两种消息体，靠**有无 jsonrpc 字段**区分：
//   1. MCP 兼容的 JSON-RPC 2.0（推荐给外部客户端）：
//        {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"__token":".."}}
//        {"jsonrpc":"2.0","id":2,"method":"tools/call",
//         "params":{"name":"nav.back","arguments":{},"__token":".."}}
//   2. 简化的本地格式（既有脚本在用，保留）：
//        {"token":"..","method":"tools/list"}
std::wstring HandleRequestLine(const std::wstring& line);

// 只走 JSON-RPC 2.0 的处理入口（已解析好的请求）。
// 单独暴露是为了让单测能直接喂结构体，不必先序列化成一行文本。
std::wstring HandleRpcLine(const json::Value& req);

}  // namespace ai
