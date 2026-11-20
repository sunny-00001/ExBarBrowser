// 诊断日志（唯一实现，架构规则 R7）。
//
// 收编理由：%TEMP%\ExBarBrowser.log 的"追加 + 超限轮转"以前有两份
//   · common.h::LogDbg      —— 给全部 UI/命令代码用
//   · shell_view.cpp::LogLine —— shell 交互诊断，逐字复制了同一段
// 两份的轮转策略还不一样（一份 4MB/500 行检查，一份完全不轮转），
// 于是 shell 那边的日志能把磁盘写满而 UI 那边不会。
//
// 这里只保留一份：LogDbg 是唯一入口（LogLine 兼容名已于 2026-11-20 删除）。
#pragma once

// printf 风格，UTF-16 追加到 %TEMP%\ExBarBrowser.log，超过 4MB 轮转为 .1。
void LogDbg(const wchar_t* fmt, ...);

// 注：曾有一个 LogLine 兼容名（shell_view.cpp 用过 31 处）。2026-11-20 已统一到
// LogDbg 并删除 —— 两个名字、零语义差异，只会让新代码不知道该用哪个。
// 校验器（tools/check_components.py）的"日志入口"规则会挡住第二个名字回来。
