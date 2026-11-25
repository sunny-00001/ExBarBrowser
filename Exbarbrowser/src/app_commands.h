// 命令注册分片的共享入口。
//
// 每个 app_commands*.cpp 开头写 `auto reg = cmd_reg::Register;`，之后照旧 reg(CmdId::X, ...)。
// 局部名字保持 reg 是有意的：tools/check_commands.py 按 "reg(CmdId::" 做静态审计，
// 换成别的名字会让"实现了却查不到"变成假失败。
#pragma once
#include <functional>
#include <string>
#include <utility>

#include "commands.h"

namespace cmd_reg {

using Handler = CommandBus::Handler;               // function<bool(const Args&)>
using ResultHandler = CommandBus::ResultHandler;   // function<bool(const Args&, json::Value*)>

inline void Register(CmdId id, Handler fn) { CommandBus::Get().Register(id, std::move(fn)); }

// 注册一个会产出结构化结果的命令（P6：读操作返回值）。
inline void RegisterWithResult(CmdId id, ResultHandler fn) {
    CommandBus::Get().RegisterWithResult(id, std::move(fn));
}

}  // namespace cmd_reg

// 分片间共享的命令辅助（定义在 app_commands_misc.cpp）：
// 打开终端（普通/管理员），tabs 分片自己也要用。
void RunTerminal(HWND owner, const std::wstring& dir, bool admin);
