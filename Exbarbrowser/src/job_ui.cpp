// 长任务进度窗口实现（接口说明见 job_ui.h）。
//
// 窗口骨架（模态循环 / 焦点归还）与 report.cpp 是同一套做法：
// 独立线程之外只有 UI 线程，等待期间必须自己泵消息，
// 且结束后要把焦点还给视图，否则 type-ahead 与 Delete 全失效。
#include "job_ui.h"

#include "config.h"   // jobMachineTimeoutMs：机器作业兜底等待（可调）
#include "identity.h"
#include "job.h"
#include "modal.h"
#include "platform_win.h"
#include "ui_draw.h"

namespace jobui {
namespace {

using id::kProgressClass;

// 轮询作业状态的间隔。100ms：跟手（进度条在动）又不至于把 CPU 吃掉。
constexpr UINT_PTR kPollTimer = 1;
constexpr int kIdCancel = 101;

// 兜底时限：只给**机器路径**用 —— 没人盯着屏幕，作业再慢也不能把界面锁死。
//
// 人侧不用它：人侧有进度条和「取消」按钮，用户随时能叫停；
// 而"等超时就把窗口关掉"是最坏的处理 —— 作业还在后台跑，用户却以为"停了"，
// 于是既看不到结果，也不敢做别的事（他不知道到底还在不在跑）。
// 具体值来自设置 jobMachineTimeoutMs（默认 120000）—— 大目录哈希/查重常超 2 分钟，
// 调用方可按自己的作业耗时调大。

struct ProgState {
    HWND hwnd = nullptr;
    HWND cancelBtn = nullptr;
    const Theme* theme = nullptr;   // 静默路径没有主题（窗口不显示，也不会绘制）
    std::wstring jobId;
    std::wstring stage;
    unsigned long long doneCount = 0;
    unsigned long long totalCount = 0;
    ULONGLONG startedAt = 0;
    // 本次最多等多久；0 = 不限时（只给人侧用）。默认取设置 jobMachineTimeoutMs，
    // 单次调用可用 WaitQuiet(timeoutMs) 覆盖（见下方赋值）。
    unsigned limitMs = static_cast<unsigned>(Config::Get().s().jobMachineTimeoutMs);
    bool isHuman = false;                  // 人侧：窗口可见、可取消
    bool done = false;
    bool cancelRequested = false;
    bool timedOut = false;
};

// 请求取消：置作业的取消位，并把界面切到"正在取消…"。
void RequestCancel(ProgState* st) {
    if (st->cancelRequested) return;
    st->cancelRequested = true;
    job::Service::Get().Cancel(st->jobId);
    if (st->cancelBtn) {
        EnableWindow(st->cancelBtn, FALSE);
        SetWindowTextW(st->cancelBtn, L"正在取消…");
    }
}

// 轮询一次作业状态。返回 false = 还可以继续等。
bool Poll(ProgState* st) {
    job::Status s;
    if (!job::Service::Get().Get(st->jobId, &s)) {
        st->done = true;
        return false;
    }
    st->stage = s.stage;
    st->doneCount = s.doneCount;
    st->totalCount = s.totalCount;
    if (s.done) {
        st->done = true;
        KillTimer(st->hwnd, kPollTimer);
        return false;
    }
    // 兜底时限：宁可放弃等，也不能让界面永远停在这个循环里。
    // 只对机器路径生效（人侧 limitMs=0）：人侧"等超时就把窗口关掉"最坏 ——
    // 作业还在跑，用户却以为停了。人要自己决定什么时候不等了。
    if (st->limitMs && st->startedAt && GetTickCount64() - st->startedAt > st->limitMs) {
        st->done = true;
        st->timedOut = true;
        KillTimer(st->hwnd, kPollTimer);
        return false;
    }
    if (st->hwnd) InvalidateRect(st->hwnd, nullptr, FALSE);
    return true;
}

// 自绘进度条 + 阶段文字。颜色一律取自 Theme（不写死像素、不写 RGB）。
void DrawProgress(HDC dc, const RECT& client, ProgState* st) {
    const Theme& th = *st->theme;
    ui::FillRectColor(dc, client, th.bar);

    std::wstring text = st->stage.empty() ? std::wstring(L"正在处理…") : st->stage;
    if (st->totalCount > 0) {
        text += L"　" + std::to_wstring(st->doneCount) + L" / " +
                std::to_wstring(st->totalCount);
    } else if (st->doneCount > 0) {
        text += L"　已处理 " + std::to_wstring(st->doneCount);
    }
    RECT tr{client.left + th.S(16), client.top + th.S(12), client.right - th.S(16),
            client.top + th.S(34)};
    ui::DrawTextRow(dc, th.font, text, tr, th.text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 进度条：底槽 + 按比例填充的色块。总量未知时不画填充（画一根满条是撒谎）。
    RECT br{client.left + th.S(16), client.top + th.S(46), client.right - th.S(16),
            client.top + th.S(46) + th.S(8)};
    ui::FillRound(dc, br, th.barAlt, th.radiusBtn());
    if (st->totalCount > 0 && st->doneCount > 0 &&
        st->totalCount <= static_cast<unsigned long long>(0x7FFFFFFF)) {
        const unsigned long long d = st->doneCount < st->totalCount ? st->doneCount
                                                                     : st->totalCount;
        const int w = static_cast<int>(MulDiv(br.right - br.left, static_cast<int>(d),
                                              static_cast<int>(st->totalCount)));
        if (w > 0) {
            RECT fr = br;
            fr.right = br.left + w;
            ui::FillRound(dc, fr, th.accent, th.radiusBtn());
        }
    }
}

LRESULT CALLBACK ProgressProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = GetThis<ProgState>(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_COMMAND: {
        if (!st) break;
        if (LOWORD(w) == kIdCancel) {
            RequestCancel(st);
            return 0;
        }
        break;
    }
    case WM_TIMER: {
        if (!st) break;
        if (w == kPollTimer) {
            Poll(st);
            return 0;
        }
        break;
    }
    case WM_CLOSE: {
        // 关窗口 = 取消。只关窗口而让作业在后台跑完，用户会以为"停了"。
        if (!st) break;
        if (!st->cancelRequested) {
            // 第一次关：先请作业停下，给它机会在下一个检查点收手。
            RequestCancel(st);
            return 0;
        }
        // 第二次关：作业已经叫不动了（卡在系统调用里），别再把用户困在窗口前。
        // 让模态循环退出，作业留在后台跑完 —— 至少用户能回到主界面干别的。
        st->done = true;
        DestroyWindow(h);
        return 0;
    }
    case WM_ERASEBKGND: {
        if (!st || !st->theme) break;
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, st->theme->brBar);
        return 1;
    }
    case WM_CTLCOLORBTN: {
        if (!st || !st->theme) break;
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, st->theme->text);
        SetBkColor(dc, st->theme->bar);
        return reinterpret_cast<LRESULT>(st->theme->brBar);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (st && st->theme) {
            RECT rc;
            GetClientRect(h, &rc);
            DrawProgress(dc, rc, st);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: {
        if (st) KillTimer(h, kPollTimer);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool WaitWithWindow(HWND owner, const Theme& theme, const std::wstring& title,
                     const std::wstring& jobId) {
    if (!win::RegisterClassOnce(kProgressClass, ProgressProc)) return false;

    ProgState st;
    st.theme = &theme;
    st.jobId = jobId;
    st.startedAt = GetTickCount64();
    st.isHuman = true;
    // 人侧不限时：进度条一直在动，用户想停就按「取消」。理由见文件头的安全时限说明。
    st.limitMs = 0;

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    const int w = theme.S(420);
    const int h = theme.S(140);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = win::CreatePopup(kProgressClass, owner, &st, WS_EX_DLGMODALFRAME,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, title.c_str());
    if (!hwnd) return false;
    st.hwnd = hwnd;

    st.cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                   w - theme.S(100), h - theme.S(44), theme.S(84), theme.S(28),
                                   hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (st.cancelBtn)
        SendMessageW(st.cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    SetTimer(hwnd, kPollTimer, 100, nullptr);
    ApplyDarkTitleBar(hwnd, theme.dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    modal::Run([&st] { return st.done; });

    if (owner) {
        EnableWindow(owner, TRUE);
        PostMessageW(owner, WM_APP_REFOCUS, 0, 0);
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    return st.cancelRequested;
}

bool WaitQuiet(const std::wstring& jobId, unsigned timeoutMs) {
    if (!win::RegisterClassOnce(kProgressClass, ProgressProc)) return false;

    ProgState st;
    st.theme = nullptr;   // 不显示窗口，也就没有主题可用（绘制分支会跳过）
    st.jobId = jobId;
    st.startedAt = GetTickCount64();
    st.isHuman = false;
    // 调用方给了时限就用它的；没给就用兜底时限（机器路径必须有上限）。
    if (timeoutMs > 0) st.limitMs = timeoutMs;

    // 消息专用窗口（HWND_MESSAGE）：只用来挂定时器，收不到任何用户输入。
    // 人工路径的窗口是模态的（owner 被禁用），机器路径不能这样 ——
    // 没人看着屏幕，却把整个界面锁住，比冻结好不到哪去。
    HWND hwnd = win::CreatePopup(kProgressClass, HWND_MESSAGE, &st, 0, WS_POPUP, 0, 0, 0, 0, L"");
    if (!hwnd) return false;
    st.hwnd = hwnd;
    SetTimer(hwnd, kPollTimer, 100, nullptr);

    modal::Run([&st] { return st.done; });

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (st.timedOut) return false;
    return st.cancelRequested;
}

}  // namespace jobui
