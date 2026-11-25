#include "kb_hook.h"

#include "log.h"
#include "str.h"

#include <atomic>

// 低层键盘钩子（WH_KEYBOARD_LL）的实现。
//
// 【这一层的唯一职责】把「输入法吃不掉」的按键事件交给 UI 线程：
//   ① 拿到**真实** vk（输入法状态下 WM_KEYDOWN 只有 VK_PROCESSKEY，没有信息量）；
//   ② 把键**吞掉**（输入法看不见 → 不会出现"按 g 弹出拼音候选框"）；
//   ③ 带上**事件流推导**的修饰键与自动重复标记，投递给 UI 线程。
// 序列状态机、热键匹配、弹窗交互全在 UI 线程（InputRouter）—— 钩子里一行业务逻辑都没有。
//
// 【为什么修饰键要自己维护】GetKeyState/GetAsyncKeyState 的结果依赖
// 「调用线程曾从消息队列处理过输入」—— 钩子线程从不处理用户输入队列，
// 在那里 GetKeyState 恒返回 0，于是任何带 Ctrl/Shift/Alt 的组合都会被判成「无修饰」
// （原版 exbar.cpp 踩过这个坑，症状是 Alt+O、Ctrl+Shift+数字全部失效）。
// 改成在钩子里跟踪修饰键的按下/抬起事件，物理状态完全由事件流推导。
//
// 【为什么吞键要连 key-up 一起吞】只吞 key-down 的话，系统仍会把 key-up 送到窗口，
// 键位状态前后不一致（视图以为按键还按着）。成对吞掉才干净。

namespace kbhook {
namespace {

HHOOK g_hook = nullptr;
HANDLE g_thread = nullptr;
DWORD g_targetThread = 0;
HWND g_mainWnd = nullptr;
HWND g_textRoot = nullptr;   // AI 面板宿主（WebView2 子树 = 文本输入区）
DWORD g_selfPid = 0;
std::atomic<bool> g_active{false};

// 钩子线程内的修饰键状态（事件流自维护，仅钩子线程读写，无需加锁）。
int g_mods = 0;

// 每个键的「按下计数」：>0 说明这个键还按着没松 —— 用来判自动重复。
//
// 【为什么要自己判重复】系统给窗口的 WM_KEYDOWN 带 bit30（自动重复标记），
// 但钩子拿不到那个标记 —— 它看到的是原始 KBDLLHOOKSTRUCT。
// 而「同一次按下的第 2 个 key-down」必然发生在 key-up 之前，
// 于是「计数 > 0 即重复」与系统的判定口径一致。
//
// 【为什么必须判】按住 g 不放时，系统会连发 key-down。
// 若当成两次独立输入，序列状态机就会拼出 "gg"（跳到第 2 个匹配项）——
// 这正是原版注释里点名的坑（「按住键产生的重复消息不能当作序列输入」）。
constexpr int kMaxTrackedVk = 256;
int g_downCount[kMaxTrackedVk] = {};

// key-down 被我们吞掉的键：对应的 key-up 也要吞（成对）。
bool g_swallowUp[kMaxTrackedVk] = {};

// 会被中文输入法吃成 VK_PROCESSKEY 的那一类键。
//
// 【只拦这些】方向键/回车/退格/F 键等输入法不碰，拦它们只是白担风险
// （还得额外处理自动重复、还得把 key-up 还回去）。字母与符号才是真正需要救的。
bool IsCharKey(unsigned vk) {
    if (vk >= '0' && vk <= '9') return true;    // 0x30-0x39
    if (vk >= 'A' && vk <= 'Z') return true;    // 0x41-0x5A
    switch (vk) {
    case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: case 0xC0:
    case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        return true;                            // OEM 1-8（; = ` [ ] \ ' 等符号键）
    default:
        return false;
    }
}

// 修饰键状态：事件流自维护（先于一切分支执行，含 injected 键，避免与真实状态失步）。
void TrackMods(unsigned vk, bool down) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        g_mods = down ? (g_mods | kCtrl) : (g_mods & ~kCtrl);
        break;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        g_mods = down ? (g_mods | kShift) : (g_mods & ~kShift);
        break;
    case VK_MENU: case VK_LMENU: case VK_RMENU:
        g_mods = down ? (g_mods | kAlt) : (g_mods & ~kAlt);
        break;
    default:
        break;
    }
}

// 当前焦点是否落在「我们管」的窗口里。
//
// 【三件套判据】
//  ① 前台窗口属于本进程 —— 否则是别的程序在用键盘，我们不能吞它的键。
//  ② 焦点窗口的根是主窗口 —— 命令面板（WS_POPUP）、各类对话框都是独立顶层窗口，
//     它们自己处理字符输入（命令面板靠 WM_CHAR 做 type-ahead），我们不能插手。
//  ③ 焦点不是 EDIT —— 地址栏/搜索框/输入对话框里的字符必须照常走输入法。
bool FocusIsOurs() {
    if (!g_mainWnd) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    DWORD fgThread = GetWindowThreadProcessId(fg, &pid);
    if (pid != g_selfPid) return false;

    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(fgThread, &gi)) return false;
    HWND focus = gi.hwndFocus;
    if (!focus) return false;

    if (GetAncestor(focus, GA_ROOT) != g_mainWnd) return false;   // ②

    wchar_t cls[32] = L"";
    GetClassNameW(focus, cls, 32);
    if (str::IEquals(cls, L"EDIT")) return false;                // ③（统一走 str.h，勿再写 _wcsicmp）

    // ③b WebView2 的输入框是 Chrome_RenderWidgetHostHWND，不是 EDIT。
    // 宿主把 AI 面板注册成 textEntryRoot 后，整棵子树都按"正在打字"处理。
    if (g_textRoot && (focus == g_textRoot || IsChild(g_textRoot, focus))) return false;
    return true;
}

LRESULT CALLBACK LowLevelKeybdProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) return CallNextHookEx(g_hook, nCode, wParam, lParam);
    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (!info) return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    TrackMods(info->vkCode, down);

    if (!IsCharKey(info->vkCode)) return CallNextHookEx(g_hook, nCode, wParam, lParam);

    // 带 Ctrl/Alt 的组合不拦：输入法不碰它们（IME 只吃裸字符），
    // 而拦了会把「Ctrl+C / Alt+F4」这类系统级组合也搅进来。
    // AltGr（右 Alt + 左 Ctrl）同理 —— 它是「输入字符」不是快捷键，交给 WM_KEYDOWN 路径。
    if (g_mods & (kCtrl | kAlt)) return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const unsigned vk = info->vkCode;
    if (vk >= kMaxTrackedVk) return CallNextHookEx(g_hook, nCode, wParam, lParam);

    if (up) {
        const bool swallow = g_swallowUp[vk];
        g_swallowUp[vk] = false;
        if (g_downCount[vk] > 0) --g_downCount[vk];
        // key-up 也吞掉：只吞 key-down 会让窗口收到"没有按下过的抬起"，
        // 键位状态前后不一致（视图会以为按键还按着）。
        if (swallow) return 1;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    if (!down) return CallNextHookEx(g_hook, nCode, wParam, lParam);
    if (!FocusIsOurs()) return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const bool repeat = g_downCount[vk] > 0;
    ++g_downCount[vk];
    g_swallowUp[vk] = true;

    auto* ev = new KeyEvent{vk, g_mods, repeat};
    if (!PostThreadMessageW(g_targetThread, kKeyMsg, 0, reinterpret_cast<LPARAM>(ev))) {
        delete ev;
    }
    // 吞键：输入法看不到这个键 —— 不会有拼音组合，也不会弹候选框。
    return 1;
}

DWORD WINAPI HookThread(LPVOID param) {
    HANDLE ready = static_cast<HANDLE>(param);
    // hMod 取本模块：低层钩子的回调必须在本进程的可执行模块里，
    // 否则装不上（SetWindowsHookEx 直接失败）。
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeybdProc, GetModuleHandleW(nullptr), 0);
    const bool ok = g_hook != nullptr;
    g_active.store(ok, std::memory_order_release);
    SetEvent(ready);
    if (!ok) {
        LogDbg(L"[kbhook] 安装失败 err=%lu（退回 WM_KEYDOWN 路径：输入法会吃掉字母）",
               GetLastError());
        return 0;
    }
    LogDbg(L"[kbhook] 已安装（投递线程 %lu）", g_targetThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_active.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

bool Start(DWORD targetThreadId, HWND mainWnd) {
    if (g_active.load(std::memory_order_acquire)) return true;
    g_targetThread = targetThreadId;
    g_mainWnd = mainWnd;
    g_selfPid = GetCurrentProcessId();
    g_mods = 0;
    for (int i = 0; i < kMaxTrackedVk; ++i) {
        g_downCount[i] = 0;
        g_swallowUp[i] = false;
    }

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    // 钩子必须装在**专用线程**上：低层钩子回调在装它的线程里被调用，
    // 而那个线程如果同时在跑消息循环，回调就要和窗口消息抢同一份队列。
    g_thread = CreateThread(nullptr, 0, HookThread, ready, 0, nullptr);
    if (!g_thread) {
        CloseHandle(ready);
        return false;
    }
    // 等钩子装好再返回：调用方（App::Run）要据此决定按键走哪条路，
    // 装到一半就返回会出现「一半键走钩子、一半键走 WM_KEYDOWN」的错乱。
    WaitForSingleObject(ready, 3000);
    CloseHandle(ready);
    return g_active.load(std::memory_order_acquire);
}

void Stop() {
    if (g_thread) {
        PostThreadMessageW(GetThreadId(g_thread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_active.store(false, std::memory_order_release);
    g_mainWnd = nullptr;
    g_targetThread = 0;
}

void SetTextEntryRoot(HWND root) { g_textRoot = root; }

bool Active() { return g_active.load(std::memory_order_acquire); }

}  // namespace kbhook
