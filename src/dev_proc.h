// 进程启动的唯一实现：**不经 shell**，可执行文件与参数分离。
//
// 【为什么单独一个文件】原来的实现在 dev_tools.cpp 里用
// `_wpopen("cd /d <repo> && <命令> 2>&1")` 拼一条 shell 命令行。
// 那条路有三个问题，一个比一个严重：
//   1. **命令注入**：命令文本是字符串拼接出来的，参数里放一个 `&& calc`
//      就是一次任意执行；
//   2. **引号不是转义**：Quote() 只在首尾加了一对双引号，引号内部的内容
//      照样被 shell 解析；
//   3. **要靠 cd**：用 `cd /d` 前缀换目录，于是整条命令都活在一层字符串里。
//
// 改成 CreateProcessW + 参数数组之后：
//   · 参数是**独立字符串**，不存在"字符串里的命令"这回事 —— 注入面直接消失；
//   · 转义按 Win32 的 CommandLineToArgvW 规则做（反斜杠 + 双引号），
//     不依赖 shell 的语法；
//   · 工作目录用 lpCurrentDirectory 指定，不需要拼前缀。
//
// 保留"收 stdout+stderr"的既有语义：两个句柄指向同一根管道。
#pragma once
#include <string>
#include <vector>

namespace devproc {

// 按 Win32（CommandLineToArgvW）规则给单个参数加引号。
// 空串 → ""；含空格/制表/引号 → 加引号并转义内部引号与尾随反斜杠。
std::wstring QuoteArg(const std::wstring& arg);

// 把 program + args 拼成一条可交给 CreateProcessW 的命令行。
std::wstring BuildCommandLine(const std::wstring& program,
                               const std::vector<std::wstring>& args);

// 在 cwd 里跑一条命令，把 stdout + stderr 收进 output（UTF-8 原文）。
//
// 返回 false 表示**进程没能启动**（err 给原因）；起来了但退出码非 0 仍返回 true，
// 退出码在 code 里 —— "跑失败"和"跑不起来"是两回事，调用方要分开报。
//
// 输出上限 kMaxOutputBytes：超了截断并**主动断开管道**，否则子进程会卡在
// 满管道上写不动，我们又在等它退出 —— 死锁。
bool RunCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                const std::wstring& cwd, int* code, std::wstring* output, std::wstring* err);

// 输出上限（字节）。截断时保留**头部与尾部**：
// 编译错误既可能出现在前面（第一处报错），也可能出现在后面（汇总），
// 只留尾巴会把第一处报错丢掉 —— 那恰恰是最该看到的一行。
constexpr size_t kMaxOutputBytes = 256 * 1024;

}  // namespace devproc
