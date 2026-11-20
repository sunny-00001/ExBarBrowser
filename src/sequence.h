// 序列键状态机（纯逻辑，零 Win32）：前缀累积 / 唯一自动执行 / 超时由调用方计时。
//
// 语义对齐 mod 的 ebch 序列段：
//   · 前缀匹配多条 -> pending，等下一键
//   · 命中且唯一（或 exact 且 seqUniqueAuto）-> 立即执行
//   · 前缀后再按不匹配的键 -> 取消（不回落成该键自己的命令）
//   · 超时（默认 3000ms）由 InputRouter 用 SetTimer 处理，本类只提供 Reset
#pragma once
#include <string>
#include <vector>

#include "keymap.h"

struct SeqHint {
    std::wstring token;   // 下一键
    std::wstring command; // 命令名
};

class SequenceMachine {
public:
    enum Result { kNoMatch = 0, kPending = 1, kMatched = 2, kAborted = -1 };

    void SetEntries(const std::vector<SequenceBinding>& entries);
    void SetUniqueAuto(bool on) { uniqueAuto_ = on; }
    void SetSingleInstant(bool on) { singleInstant_ = on; }

    void Reset();
    bool pending() const { return !prefix_.empty(); }
    std::wstring prefix() const;      // "g" / "x l"
    size_t candidateCount() const;

    std::vector<SeqHint> Hints() const;

    // token 已规范化（小写字母/数字/符号）
    Result Feed(const std::wstring& token, std::wstring* cmdOut);

    // 超时兜底：`x l` 之后还有 `x l a` 时，先挂起完整匹配；超时未续按则执行它。
    bool TakePendingExact(std::wstring* cmdOut);

private:
    struct Entry {
        std::vector<std::wstring> tokens;
        std::wstring command;
    };
    std::vector<Entry> entries_;
    std::vector<std::wstring> prefix_;
    std::wstring pendingExact_;  // 完整匹配但仍有更长候选时挂起
    bool uniqueAuto_ = true;
    bool singleInstant_ = true;
};
