#include "shell_op_exec.h"

namespace shellopexec {

std::wstring HexHr(HRESULT hr) {
    static const wchar_t* kDigits = L"0123456789ABCDEF";
    unsigned v = static_cast<unsigned>(hr);
    wchar_t buf[9];
    for (int i = 7; i >= 0; --i) {
        buf[i] = kDigits[v & 0xF];
        v >>= 4;
    }
    buf[8] = L'\0';
    return std::wstring(buf);
}

bool Run(IFileOperation* op, std::wstring* why) {
    if (!op) {
        if (why) *why = L"文件操作对象创建失败";
        return false;
    }
    const HRESULT hr = op->PerformOperations();
    if (FAILED(hr)) {
        if (why) *why = L"文件操作失败（HRESULT=0x" + HexHr(hr) + L"）";
        LogDbg(L"[shellopexec] PerformOperations 失败 hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    BOOL aborted = FALSE;
    const HRESULT hr2 = op->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr2)) {
        if (why) *why = L"文件操作状态查询失败";
        return false;
    }
    if (aborted) {
        // S_OK 但 aborted=TRUE：通常是目标在过程中消失、或被别的程序占用。
        if (why) *why = L"文件操作被中止（目标可能被占用或已消失）";
        return false;
    }
    return true;
}

}  // namespace shellopexec
