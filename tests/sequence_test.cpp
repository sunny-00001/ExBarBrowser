// 序列键状态机自测（纯逻辑，不依赖 Win32）：
//   clang++ -std=c++20 -O1 -Isrc tests/sequence_test.cpp src/sequence.cpp -o /tmp/seq_test.exe
#include <cstdio>
#include <string>
#include <vector>

#include "sequence.h"

namespace {

int g_fail = 0;

void Expect(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

SequenceBinding B(const wchar_t* seq, const wchar_t* cmd) {
    SequenceBinding b;
    b.seq = seq;
    b.tokens = SplitSequence(seq);
    b.command = cmd;
    return b;
}

}  // namespace

int main() {
    SequenceMachine m;
    m.SetEntries({B(L"g g", L"FileFocusTop"), B(L"g t", L"TabNew"), B(L"x l", L"CloseLeftOne"),
                  B(L"x l a", L"TabCloseLeft")});

    std::wstring cmd;
    Expect(m.Feed(L"g", &cmd) == SequenceMachine::kPending, "g -> pending");
    Expect(m.Feed(L"g", &cmd) == SequenceMachine::kMatched && cmd == L"FileFocusTop",
           "g g -> FileFocusTop");

    cmd.clear();
    Expect(m.Feed(L"x", &cmd) == SequenceMachine::kPending, "x -> pending");
    Expect(m.Feed(L"l", &cmd) == SequenceMachine::kPending, "x l -> pending (x l a 更长)");
    Expect(m.TakePendingExact(&cmd) && cmd == L"CloseLeftOne", "timeout -> CloseLeftOne");

    cmd.clear();
    m.Feed(L"x", &cmd);
    m.Feed(L"l", &cmd);
    Expect(m.Feed(L"a", &cmd) == SequenceMachine::kMatched && cmd == L"TabCloseLeft",
           "x l a -> TabCloseLeft");

    cmd.clear();
    m.Feed(L"g", &cmd);
    Expect(m.Feed(L"z", &cmd) == SequenceMachine::kAborted, "g z -> aborted");
    Expect(!m.pending(), "abort clears prefix");

    // 前缀提示：g 之后应有 g/t 两个候选
    m.Reset();
    m.Feed(L"g", &cmd);
    auto hints = m.Hints();
    Expect(hints.size() == 2 && hints[0].token == L"g" && hints[1].token == L"t",
           "g hints = {g,t}");

    std::printf("%s\n", g_fail ? "SEQUENCE TEST FAILED" : "SEQUENCE TEST OK");
    return g_fail ? 1 : 0;
}
