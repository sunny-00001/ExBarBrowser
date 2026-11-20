#include "bookmark_drop.h"

#include "bookmark_bar.h"
#include "bookmarks.h"

#include <shlobj.h>

namespace {

// 屏幕点 → 该处文件夹书签的 id（拖到文件夹按钮上=加入其内部；返回空=加到顶层）。
// BookmarkBar 提供 hit 回调，避免 drop target 直接摸它的内部布局。
std::wstring FolderIdAt(BookmarkBar* owner, POINT screen) {
    if (!owner) return std::wstring();
    int idx = owner->FolderItemAt(screen);  // -1 = 无项
    if (idx < 0) return std::wstring();
    return owner->FolderIdAt(idx);
}

// IDropTarget 的 POINTL 是目标窗口客户区坐标；BookmarkBar::FolderItemAt 收屏幕坐标。
std::wstring FolderIdAtClient(HWND hwnd, BookmarkBar* owner, POINTL pt) {
    POINT p{pt.x, pt.y};
    if (hwnd) ClientToScreen(hwnd, &p);
    return FolderIdAt(owner, p);
}

std::wstring PidlToPathOrNamespace(LPCITEMIDLIST pidl) {
    if (!pidl) return std::wstring();
    // 优先 shell 解析名：虚拟项（此电脑/网络…）与命名空间扩展保持 ::{GUID} 形态，
    // 导航与图标解析都认这个。
    PWSTR pns = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &pns)) && pns && *pns) {
        std::wstring ns = pns;
        CoTaskMemFree(pns);
        if (ns.rfind(L"::{", 0) == 0) return ns;
        // 非命名空间解析名只做兜底，先试文件系统路径。
        wchar_t path[MAX_PATH * 2] = L"";
        if (SHGetPathFromIDListW(pidl, path) && path[0]) return path;
        return ns;
    }
    wchar_t path[MAX_PATH * 2] = L"";
    if (SHGetPathFromIDListW(pidl, path) && path[0]) return path;
    return std::wstring();
}

class DropTarget : public IDropTarget {
public:
    DropTarget() : ref_(1) {}
    virtual ~DropTarget() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL pt,
                                        DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        // 保留数据对象支持的 effect 位：虚拟项常是 MOVE|LINK，强制 COPY 会被系统
        // 对 LINK-only 项取消拖放（mod 同款处理）。
        *pdwEffect &= (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        hoverFolder_ = FolderIdAtClient(hwnd_, owner_, pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect &= (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        hoverFolder_ = FolderIdAtClient(hwnd_, owner_, pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        hoverFolder_.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pdo, DWORD, POINTL pt,
                                   DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        // Drop 的落点才是最终容器；不依赖 DragOver 的最后一次坐标。
        std::wstring folder = FolderIdAtClient(hwnd_, owner_, pt);
        hoverFolder_.clear();
        ExtractAndAdd(pdo, folder);
        return S_OK;
    }

    void Set(HWND h, BookmarkBar* owner) {
        hwnd_ = h;
        owner_ = owner;
    }

private:
    void ExtractAndAdd(IDataObject* pdo, const std::wstring& folderId) {
        if (!pdo) return;
        BookmarkStore& store = BookmarkStore::Get();

        // 首选 CFSTR_SHELLIDLIST（shell 拖放标准格式，含 CIDA PIDL 数组）。
        UINT cf = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        FORMATETC fe = {static_cast<CLIPFORMAT>(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm = {};
        if (SUCCEEDED(pdo->GetData(&fe, &sm))) {
            CIDA* cida = static_cast<CIDA*>(GlobalLock(sm.hGlobal));
            if (cida && cida->cidl > 0) {
                // aoffset[0]=父 PIDL，aoffset[1..cidl]=子项（相对父），ILCombine 拼全。
                LPCITEMIDLIST parent =
                    reinterpret_cast<LPCITEMIDLIST>(reinterpret_cast<BYTE*>(cida) +
                                                    cida->aoffset[0]);
                int added = 0;
                for (UINT i = 1; i <= cida->cidl; ++i) {
                    LPCITEMIDLIST child =
                        reinterpret_cast<LPCITEMIDLIST>(reinterpret_cast<BYTE*>(cida) +
                                                        cida->aoffset[i]);
                    PIDLIST_ABSOLUTE full = ILCombine(parent, child);
                    if (!full) continue;
                    std::wstring p = PidlToPathOrNamespace(full);
                    if (!p.empty() && !store.ContainsPath(p)) {
                        store.AddPath(NameFor(p), p, folderId);
                        ++added;
                    }
                    CoTaskMemFree(full);
                }
                if (added) {
                    store.Save();
                    owner_->ReloadExternal();
                }
            }
            if (cida) GlobalUnlock(sm.hGlobal);
            ReleaseStgMedium(&sm);
            return;
        }

        // 回退：部分虚拟项不从 IDataObject 给 IDList，用 SHGetIDListFromObject。
        PIDLIST_ABSOLUTE pidlObj = nullptr;
        if (SUCCEEDED(SHGetIDListFromObject(pdo, &pidlObj)) && pidlObj) {
            std::wstring p = PidlToPathOrNamespace(pidlObj);
            if (!p.empty() && !store.ContainsPath(p)) {
                store.AddPath(NameFor(p), p, folderId);
                store.Save();
                owner_->ReloadExternal();
            }
            CoTaskMemFree(pidlObj);
        }
    }

    static std::wstring NameFor(const std::wstring& path) {
        // 命名空间项用 GUID 前截断没有意义，显示名交给图标解析失败时的 fallback；
        // 文件系统路径取末段。
        if (path.rfind(L"::{", 0) == 0) return path;
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    ULONG ref_ = 1;
    HWND hwnd_ = nullptr;
    BookmarkBar* owner_ = nullptr;
    std::wstring hoverFolder_;
};

DropTarget* g_drop = nullptr;

}  // namespace

void BmkDropAttach(HWND bar, BookmarkBar* owner) {
    if (!bar || g_drop) return;
    DropTarget* t = new DropTarget();
    t->Set(bar, owner);
    if (SUCCEEDED(RegisterDragDrop(bar, t))) {
        g_drop = t;  // 窗口持一份引用
    } else {
        t->Release();
    }
}

void BmkDropDetach(HWND bar) {
    if (!bar || !g_drop) return;
    RevokeDragDrop(bar);
    g_drop->Release();  // 配对 RegisterDragDrop 成功时的引用
    g_drop = nullptr;
}
