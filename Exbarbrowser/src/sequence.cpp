#include "sequence.h"

#include <algorithm>

void SequenceMachine::SetEntries(const std::vector<SequenceBinding>& entries) {
    entries_.clear();
    for (const auto& b : entries) {
        if (b.tokens.empty() || b.command.empty()) continue;
        entries_.push_back({b.tokens, b.command});
    }
    Reset();
}

void SequenceMachine::Reset() {
    prefix_.clear();
    pendingExact_.clear();
}

bool SequenceMachine::TakePendingExact(std::wstring* cmdOut) {
    if (pendingExact_.empty()) return false;
    if (cmdOut) *cmdOut = pendingExact_;
    pendingExact_.clear();
    prefix_.clear();
    return true;
}

std::wstring SequenceMachine::prefix() const {
    std::wstring s;
    for (const auto& t : prefix_) {
        if (!s.empty()) s += L" ";
        s += t;
    }
    return s;
}

namespace {

bool StartsWith(const std::vector<std::wstring>& tokens, const std::vector<std::wstring>& prefix) {
    if (prefix.size() > tokens.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (tokens[i] != prefix[i]) return false;
    return true;
}

}  // namespace

size_t SequenceMachine::candidateCount() const {
    size_t n = 0;
    for (const auto& e : entries_)
        if (StartsWith(e.tokens, prefix_)) ++n;
    return n;
}

std::vector<SeqHint> SequenceMachine::Hints() const {
    std::vector<SeqHint> out;
    if (prefix_.empty()) return out;
    for (const auto& e : entries_) {
        if (!StartsWith(e.tokens, prefix_)) continue;
        if (e.tokens.size() <= prefix_.size()) continue;
        const std::wstring& next = e.tokens[prefix_.size()];
        bool dup = false;
        for (const auto& h : out)
            if (h.token == next) {
                dup = true;
                break;
            }
        if (!dup) out.push_back({next, e.command});
    }
    std::sort(out.begin(), out.end(),
              [](const SeqHint& a, const SeqHint& b) { return a.token < b.token; });
    return out;
}

SequenceMachine::Result SequenceMachine::Feed(const std::wstring& token, std::wstring* cmdOut) {
    if (entries_.empty() || token.empty()) return kNoMatch;
    if (cmdOut) cmdOut->clear();

    std::vector<std::wstring> next = prefix_;
    next.push_back(token);

    bool any = false;
    bool exact = false;
    const Entry* exactEntry = nullptr;
    size_t matches = 0;
    for (const auto& e : entries_) {
        if (!StartsWith(e.tokens, next)) continue;
        any = true;
        ++matches;
        if (e.tokens.size() == next.size()) {
            exact = true;
            exactEntry = &e;
        }
    }

    if (!any) {
        // 前缀后按错键：整体取消（不把该键当普通命令，避免误触）
        if (!prefix_.empty()) {
            prefix_.clear();
            pendingExact_.clear();
            return kAborted;
        }
        return kNoMatch;
    }

    if (exact) {
        // 只有"没有更长候选"的完整匹配才能立即执行。
        // 否则 `x l` 会在 `x l a` 还没按完时抢先触发（matches>1 必须继续等）。
        if (exactEntry && matches == 1) {
            prefix_.clear();
            pendingExact_.clear();
            if (cmdOut) *cmdOut = exactEntry->command;
            return kMatched;
        }
        pendingExact_ = exactEntry ? exactEntry->command : std::wstring();
        prefix_ = std::move(next);
        return kPending;
    }

    // 非完整匹配：唯一候选且开启 uniqueAuto 时直接执行（mod 的"唯一候选自动执行"）
    if (matches == 1 && uniqueAuto_ && singleInstant_) {
        const Entry* only = nullptr;
        for (const auto& e : entries_)
            if (StartsWith(e.tokens, next)) {
                only = &e;
                break;
            }
        if (only) {
            prefix_.clear();
            pendingExact_.clear();
            if (cmdOut) *cmdOut = only->command;
            return kMatched;
        }
    }

    pendingExact_.clear();
    prefix_ = std::move(next);
    return kPending;
}
