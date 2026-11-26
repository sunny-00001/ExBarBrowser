#include "shell_op_exec.h"

namespace shellopexec {
namespace {

// IFileOperationProgressSink：只为"数失败"而存在。
//
// 为什么必须装它：PerformOperations 返回 S_OK、GetAnyOperationsAborted 为 FALSE
// 只代表"用户没点取消"。个别项失败（文件被占用、权限不足、路径过长）时
// shell 会跳过该项、其余照做 —— 这时两个判据都是"成功"，只有
// Post*Item 的 hrResult 能看出"这一项没做成"。
//
// 所有回调一律返回 S_OK：接收器只**观察**，不干预（返回失败会中止整个操作，
// 那是另一种语义，不是这里要的）。
class FailureSink : public IFileOperationProgressSink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override {
        ++attempted_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr,
                                             IShellItem*) override {
        return Note(hr);
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override {
        ++attempted_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr,
                                           IShellItem*) override {
        return Note(hr);
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override {
        ++attempted_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr,
                                           IShellItem*) override {
        return Note(hr);
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override {
        ++attempted_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT hr,
                                             IShellItem*) override {
        return Note(hr);
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override {
        ++attempted_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr,
                                          IShellItem*) override {
        return Note(hr);
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

    unsigned attempted() const { return attempted_; }
    unsigned failed() const { return failed_; }
    HRESULT firstFail() const { return firstFail_; }

private:
    HRESULT Note(HRESULT hr) {
        if (FAILED(hr)) {
            ++failed_;
            if (SUCCEEDED(firstFail_)) firstFail_ = hr;
        }
        return S_OK;
    }

    LONG ref_ = 1;
    unsigned attempted_ = 0;
    unsigned failed_ = 0;
    HRESULT firstFail_ = S_OK;
};

}  // namespace

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

Op::~Op() = default;

bool Op::Begin(HWND owner, DWORD flags, std::wstring* why) {
    const HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                        IID_IFileOperation, reinterpret_cast<void**>(&op_));
    if (FAILED(hr) || !op_) {
        if (why) *why = L"无法创建文件操作对象（HRESULT=0x" + HexHr(hr) + L"）";
        LogDbg(L"[shellopexec] CoCreateInstance 失败 hr=0x%08X", static_cast<unsigned>(hr));
        op_.Reset();
        return false;
    }
    // 标志必须判：FOF_ALLOWUNDO 决定删除进不进回收站。设不上却继续执行，
    // 就是"以为能撤销、实际永久删除"，且界面上没有任何提示。
    const HRESULT hrFlags = op_->SetOperationFlags(flags);
    if (FAILED(hrFlags)) {
        if (why) *why = L"无法设置文件操作选项（HRESULT=0x" + HexHr(hrFlags) + L"）";
        LogDbg(L"[shellopexec] SetOperationFlags 失败 hr=0x%08X flags=0x%08X",
               static_cast<unsigned>(hrFlags), static_cast<unsigned>(flags));
        op_.Reset();
        return false;
    }
    // owner 只影响对话框归属，失败不改变操作语义 —— 记日志即可，不中止。
    const HRESULT hrOwner = op_->SetOwnerWindow(owner);
    if (FAILED(hrOwner))
        LogDbg(L"[shellopexec] SetOwnerWindow 失败 hr=0x%08X（不影响操作）",
               static_cast<unsigned>(hrOwner));
    return true;
}

// 排队结果的唯一记账点：所有 Queue* 都走这里。
bool Op::NoteQueue(HRESULT hr, const std::wstring& label) {
    if (SUCCEEDED(hr)) {
        ++queued_;
        return true;
    }
    ++queueFailed_;
    if (SUCCEEDED(firstQueueFail_)) {
        firstQueueFail_ = hr;
        firstQueueFailLabel_ = label;
    }
    return false;
}

bool Op::QueueDelete(IShellItem* item, const std::wstring& label) {
    if (!op_ || !item) return NoteQueue(E_INVALIDARG, label);
    return NoteQueue(op_->DeleteItem(item, nullptr), label);
}

bool Op::QueueRename(IShellItem* item, const wchar_t* newName, const std::wstring& label) {
    if (!op_ || !item) return NoteQueue(E_INVALIDARG, label);
    return NoteQueue(op_->RenameItem(item, newName, nullptr), label);
}

bool Op::QueueMove(IShellItem* item, IShellItem* destDir, const wchar_t* newName,
                   const std::wstring& label) {
    if (!op_ || !item || !destDir) return NoteQueue(E_INVALIDARG, label);
    return NoteQueue(op_->MoveItem(item, destDir, newName, nullptr), label);
}

bool Op::QueueCopy(IShellItem* item, IShellItem* destDir, const wchar_t* newName,
                   const std::wstring& label) {
    if (!op_ || !item || !destDir) return NoteQueue(E_INVALIDARG, label);
    return NoteQueue(op_->CopyItem(item, destDir, newName, nullptr), label);
}

bool Op::QueueMoveAll(IShellItemArray* items, IShellItem* destDir) {
    if (!op_ || !items || !destDir) return NoteQueue(E_INVALIDARG, L"");
    return NoteQueue(op_->MoveItems(items, destDir), L"");
}

bool Op::QueueCopyAll(IShellItemArray* items, IShellItem* destDir) {
    if (!op_ || !items || !destDir) return NoteQueue(E_INVALIDARG, L"");
    return NoteQueue(op_->CopyItems(items, destDir), L"");
}

bool Op::QueueNewItem(IShellItem* destFolder, DWORD attrs, const wchar_t* name) {
    if (!op_ || !destFolder) return NoteQueue(E_INVALIDARG, name ? name : L"");
    return NoteQueue(op_->NewItem(destFolder, attrs, name, nullptr, nullptr), name ? name : L"");
}

bool Op::Commit(std::wstring* why) {
    if (!op_) {
        if (why) *why = L"文件操作对象未就绪";
        return false;
    }
    // ④ 一项都没排上 = 排队阶段就全失败了。此时队列是空的，
    // PerformOperations 会返回 S_OK —— 不在这里拦，就是假成功。
    if (queued_ == 0) {
        std::wstring detail = L"没有可执行的文件操作";
        if (queueFailed_ > 0) {
            detail = L"所有项都无法排入队列（" + std::to_wstring(queueFailed_) +
                     L" 项失败，HRESULT=0x" + HexHr(firstQueueFail_) + L"）";
            if (!firstQueueFailLabel_.empty()) detail += L"：" + firstQueueFailLabel_;
        }
        if (why) *why = detail;
        LogDbg(L"[shellopexec] %s", detail.c_str());
        return false;
    }

    // ③ 逐项结果：装接收器，把"部分失败"也数出来。
    FailureSink* sink = new FailureSink();
    DWORD cookie = 0;
    const HRESULT hrAdvise = op_->Advise(sink, &cookie);
    if (FAILED(hrAdvise)) {
        // 装不上接收器不是致命错误：①②④ 仍能挡住大部分失败，
        // 只是"部分失败"会漏 —— 记日志让这类情况可追。
        LogDbg(L"[shellopexec] Advise 进度接收器失败 hr=0x%08X（部分失败将不可见）",
               static_cast<unsigned>(hrAdvise));
        cookie = 0;
    }

    const HRESULT hr = op_->PerformOperations();

    // ① PerformOperations
    if (FAILED(hr)) {
        if (why) *why = L"文件操作失败（HRESULT=0x" + HexHr(hr) + L"）";
        LogDbg(L"[shellopexec] PerformOperations 失败 hr=0x%08X", static_cast<unsigned>(hr));
        if (cookie) op_->Unadvise(cookie);
        sink->Release();
        return false;
    }

    // ② 用户取消
    BOOL aborted = FALSE;
    const HRESULT hr2 = op_->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr2)) {
        if (why) *why = L"文件操作状态查询失败";
        if (cookie) op_->Unadvise(cookie);
        sink->Release();
        return false;
    }
    if (aborted) {
        // S_OK 但 aborted=TRUE：用户点了取消，或目标在过程中消失。
        if (why) *why = L"文件操作被中止（目标可能被占用或已消失）";
        if (cookie) op_->Unadvise(cookie);
        sink->Release();
        return false;
    }

    // ③ 部分失败：逐项 hrResult 里 FAILED 的项。
    const unsigned failedItems = sink->failed();
    const unsigned attempted = sink->attempted();
    const HRESULT firstItemFail = sink->firstFail();
    if (cookie) op_->Unadvise(cookie);
    sink->Release();

    if (failedItems > 0) {
        std::wstring detail = L"文件操作部分失败（" + std::to_wstring(failedItems) + L" / " +
                              std::to_wstring(attempted) + L" 项失败，HRESULT=0x" +
                              HexHr(firstItemFail) + L"）";
        if (why) *why = detail;
        LogDbg(L"[shellopexec] %s", detail.c_str());
        return false;
    }
    return true;
}

}  // namespace shellopexec
