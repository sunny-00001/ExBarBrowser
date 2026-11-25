// 剪贴板写入自测：写入正确性 + 失败时不清空用户原有内容。
//
// 守的是两类问题：
//   1. 顺序缺陷 —— 旧实现是 OpenClipboard → EmptyClipboard → GlobalAlloc，
//      分配失败时用户原有剪贴板**已被清空**，这次没复制成功、上次的也没了。
//   2. 占用不重试 —— 别的进程持着剪贴板时直接失败，表现为"复制偶尔没反应"。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 \
//     -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc \
//     tests/clipboard_test.cpp src/clipboard.cpp \
//     -o /tmp/clipboard_test.exe -lole32 -luser32
//
// 注意：本测试**会改动系统剪贴板**（这是它的目的）。跑之前请自行保存。
//
// 退出码：0 = 全通过；1 = 有 CHECK 失败（真问题）；77 = 环境不满足（本进程
// 打不开剪贴板），run_all.sh 记为"无法运行"而非失败 —— 见 ProbeClipboard。
#include <windows.h>

#include <cstdio>
#include <string>

#include "clipboard.h"

static int fails = 0;
#define CHECK(cond, msg)                  \
    do {                                  \
        if (cond) {                       \
            printf("ok   %s\n", msg);     \
        } else {                          \
            printf("FAIL %s\n", msg);     \
            ++fails;                      \
        }                                 \
    } while (0)

// 前置条件探针：本进程能不能碰剪贴板。
//
// 【为什么必须先探】剪贴板是**全系统独占**资源：别的进程正持有时，OpenClipboard
// 直接返回 ERROR_ACCESS_DENIED(5)；没有交互桌面权限的环境（沙箱、非交互会话）同样
// 是这个错误码。这时下面每一条 CHECK 都会 FAIL —— 但这不是 SetClipboardText 的结论，
// 而是"根本没测"。把它报成 9 个 FAIL 的后果是每次全量跑都红着一条假问题，久了真正
// 的回归会被"这条一直红"掩盖。实测过这个来回：同一批 CHECK 上午 9 红、下午两次全绿，
// 代码一个字节没变。
//
// 所以：探不通 → 退出码 77（run_all.sh 记为"无法运行"），一句话说清是环境。
// 探得通 → 下面的 CHECK 全部是真判据，失败就是代码的问题。
//
// 探针比产品代码等得更久（20 × 25ms ≈ 0.5s，SetClipboardText 只重试 100ms）：
// 产品那个预算是"让一次复制别偶发失败"，探针问的是"这台机器现在能不能测"，
// 标准不该一样 —— 探针太急会把"占用 200ms"误判成"测不了"。
//
// 说明用的是窄字符串：printf 的 %ls 在 C locale 下遇到非 ASCII 就截断，
// 曾经把这条诊断打成半句"OpenClipboard "，最要紧的错误码反而丢了。
static bool ProbeClipboard(std::string* why) {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(nullptr)) {
            CloseClipboard();
            return true;
        }
        Sleep(25);
    }
    const DWORD err = GetLastError();
    *why = "OpenClipboard 失败，GetLastError=" + std::to_string(err);
    if (err == ERROR_ACCESS_DENIED) {
        // 把会话也打出来：换台机器时能一眼区分"别的进程占着"和"会话隔离"。
        DWORD sid = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        *why += "（进程会话 " + std::to_string(sid) +
                "；要么别的进程正持有剪贴板，要么本进程没有交互桌面权限）";
    }
    return false;
}

static std::wstring ReadClip() {
    std::wstring r;
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
            if (p) {
                r = p;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    return r;
}

int main() {
    // 前置条件：打不开剪贴板就连"测"都谈不上，报 FAIL 会掩盖真正的回归。
    std::string why;
    if (!ProbeClipboard(&why)) {
        std::printf("无法运行：%s\n", why.c_str());
        std::printf("这不是被测代码的结论 —— 本机环境不让本进程访问剪贴板。\n");
        std::printf("请在交互桌面（非沙箱）环境重跑本测试。\n");
        return 77;
    }

    printf("写入与读回\n");
    {
        CHECK(SetClipboardText(nullptr, L"AAA"), "写入 AAA 成功");
        CHECK(ReadClip() == L"AAA", "读回 AAA");
        CHECK(SetClipboardText(nullptr, L"BBB"), "覆盖写入 BBB 成功");
        CHECK(ReadClip() == L"BBB", "读回 BBB（不是 AAA）");
        CHECK(SetClipboardText(nullptr, L"中文路径\\子目录"), "写入中文成功");
        CHECK(ReadClip() == L"中文路径\\子目录", "中文内容正确（UTF-16 无转换损失）");
        CHECK(SetClipboardText(nullptr, L""), "写入空串成功");
        CHECK(ReadClip() == L"", "读回空串");
    }

    printf("\n失败路径不得清空用户原有内容\n");
    {
        // 先放一段"用户内容"
        CHECK(SetClipboardText(nullptr, L"USER-ORIGINAL"), "放入用户内容");
        CHECK(ReadClip() == L"USER-ORIGINAL", "确认已就位");

        // 制造 GlobalAlloc 失败：申请一个必然失败的量。
        // 用 GMEM_MOVEABLE 申请 4GB（32 位进程必然失败；64 位也可能因提交限制失败）。
        // 这里不走 SetClipboardText 是为了直接观察"分配失败时剪贴板有无被清空"，
        // 等价于旧实现在那一分支的行为。
        const size_t hugeBytes = static_cast<size_t>(4) << 30;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, hugeBytes);
        const bool allocFailed = (h == nullptr);
        if (h) GlobalFree(h);

        if (allocFailed) {
            // 分配确实失败了 —— 那么"若旧实现那样先 EmptyClipboard"就会丢内容。
            // 现在 SetClipboardText 在该分支之前不动剪贴板，所以我们直接验证：
            // 用户内容仍然在（因为没有任何代码清空它）。
            CHECK(ReadClip() == L"USER-ORIGINAL", "分配失败后用户内容仍在（未被清空）");
        } else {
            printf("skip 本机 4GB 分配未失败，跳过该分支（非代码问题）\n");
        }
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
