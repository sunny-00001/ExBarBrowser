// 「再点即收」的状态：只记"当前哪一个锚点的弹层开着"。机制见 popup_toggle.h。
//
// 这里**没有时间戳**。第一版靠 400ms 窗口判连点，实测无效 —— 用户是"看一眼再点"，
// 间隔远超窗口，且 WM_CLOSE 是投递的、判据到手时已经过期。留着时间戳只会诱使
// 后来人再写一次"关闭后猜"的逻辑，所以彻底删掉。
#include "popup_toggle.h"

namespace popup {
namespace {

// 单槽：弹层是模态的，同一时刻只会有一个开着，因此"当前打开的是哪个锚点"
// 恰好就是一个字符串。不需要表、不需要栈、不需要多槽。
struct Slot {
    std::wstring key;
    bool valid = false;
};

Slot& Get() {
    static Slot s;
    return s;
}

}  // namespace

void NoteOpened(const wchar_t* key) {
    if (!key || !*key) return;
    Slot& s = Get();
    s.key = key;
    s.valid = true;
}

void NoteClosed(const wchar_t* key) {
    Slot& s = Get();
    if (!s.valid) return;
    // 只注销"自己那一个"：嵌套调用（例如颜色二级菜单）里外层注销时，
    // 不能把内层刚登记的记录误清掉。key 为空视为"无条件注销"。
    if (!key || !*key || s.key == key) {
        s.valid = false;
        s.key.clear();
    }
}

bool IsOpen(const wchar_t* key) { return IsOpen(key, nullptr); }

bool IsOpen(const wchar_t* key, std::wstring* openKey) {
    const Slot& s = Get();
    if (openKey) *openKey = s.valid ? s.key : std::wstring();
    if (!key || !*key) return false;
    return s.valid && s.key == key;
}

std::wstring OpenKey() {
    const Slot& s = Get();
    return s.valid ? s.key : std::wstring();
}

void Reset() {
    Slot& s = Get();
    s.valid = false;
    s.key.clear();
}

// 判据与理由见 popup_toggle.h。两条都不成立才关；持有捕获 = 这一次点击
// 正被我们吞着，等 UP 再决定，绝不在这里提前销毁窗口。
bool ShouldCloseOnDeactivate(bool holdsCapture, bool foregroundSameThread) {
    if (!holdsCapture) return true;
    return !foregroundSameThread;
}

bool ShouldCloseOnDeactivate(HWND self, HWND list) {
    // 捕获必须真的还在我们手里：被别的窗口抢走后就是"关不掉"的状态，
    // 这时失活必须关（WM_CAPTURECHANGED 那条路也会关，这里兜第二重）。
    const bool holdsCapture = list != nullptr && GetCapture() == list;

    // 前景窗与弹层同线程 = 点的是本程序自己的另一个窗口（书签栏/标签/状态栏）。
    // 跨进程点击（另一个程序抢焦点）拿不到同线程前景窗，回落为"关"。
    const HWND fg = GetForegroundWindow();
    bool sameThread = false;
    if (fg && self) {
        const DWORD fgTid = GetWindowThreadProcessId(fg, nullptr);
        const DWORD selfTid = GetWindowThreadProcessId(self, nullptr);
        sameThread = fgTid != 0 && fgTid == selfTid;
    }
    return ShouldCloseOnDeactivate(holdsCapture, sameThread);
}

}  // namespace popup
