// 剪贴板文本写入实现（唯一）。见 clipboard.h。
#include "clipboard.h"

#include <windows.h>

namespace {

// 剪贴板是**全系统共享、同一时刻只能被一方打开**的资源。别的进程正持有时
// OpenClipboard 直接失败，这是常态而非异常 —— 用户复制的瞬间可能刚好被
// 输入法、取词工具、剪贴板管理器占着。原来不重试，表现为"复制路径偶尔没反应"。
bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(20);   // 5 × 20ms：够跨过一次短占用，又不会让 UI 明显卡顿
    }
    return false;
}

}  // namespace

bool SetClipboardText(void* owner, const std::wstring& text) {
    HWND hwnd = static_cast<HWND>(owner);
    if (!OpenClipboardRetry(hwnd)) return false;

    // 【顺序很关键】先把内容准备好，再清空剪贴板。
    //
    // 原实现是 OpenClipboard → EmptyClipboard → GlobalAlloc。分配失败时
    // （内存紧张、text 极大）走的是 "CloseClipboard + return false"，
    // 而用户原有的剪贴板内容**已经被清空** —— 这次复制没成功，
    // 上一次复制的东西也没了，两头空。
    //
    // 改成先分配 + 拷贝，全部成功后才 EmptyClipboard + SetClipboardData。
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(mem);

    // 到这里才有资格动剪贴板里已有的内容。
    EmptyClipboard();
    // 成功交给剪贴板后所有权归系统；失败必须自己释放，否则每次复制漏一块。
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}
