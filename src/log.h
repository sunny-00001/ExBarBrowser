// 诊断日志（唯一实现，架构规则 R7）。
//
// 收编理由：%TEMP%\ExBarBrowser.log 的"追加 + 超限轮转"以前有两份
//   · common.h::LogDbg      —— 给全部 UI/命令代码用
//   · shell_view.cpp::LogLine —— shell 交互诊断，逐字复制了同一段
// 两份的轮转策略还不一样（一份 4MB/500 行检查，一份完全不轮转），
// 于是 shell 那边的日志能把磁盘写满而 UI 那边不会。
//
// 这里只保留一份：LogDbg 是唯一入口，LogLine 作为兼容名转发。
#pragma once

// printf 风格，UTF-16 追加到 %TEMP%\ExBarBrowser.log，超过 4MB 轮转为 .1。
void LogDbg(const wchar_t* fmt, ...);

// 兼容名：既有 shell_view.cpp 里的调用点用这个名字，语义完全相同。
void LogLine(const wchar_t* fmt, ...);
