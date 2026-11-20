// 剪贴板文本写入实现（唯一）。见 clipboard.h。
#include "clipboard.h"

#include <windows.h>

bool SetClipboardText(void* owner, const std::wstring& text) {
    HWND hwnd = static_cast<HWND>(owner);
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();

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
    // 成功交给剪贴板后所有权归系统；失败必须自己释放，否则每次复制漏一块。
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}
