#include "bookmark_drop.h"

#include "bookmark_bar.h"
#include "log.h"

#include <shlobj.h>

#include <vector>

namespace {

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

std::vector<std::wstring> ExtractPaths(IDataObject* pdo) {
    std::vector<std::wstring> paths;
    if (!pdo) return paths;

    // 首选 CFSTR_SHELLIDLIST（shell 拖放标准格式，含 CIDA PIDL 数组）。
    const UINT cf = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    FORMATETC fe = {static_cast<CLIPFORMAT>(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {};
    if (SUCCEEDED(pdo->GetData(&fe, &sm))) {
        CIDA* cida = static_cast<CIDA*>(GlobalLock(sm.hGlobal));
        if (cida && cida->cidl > 0) {
            // aoffset[0]=父 PIDL，aoffset[1..cidl]=子项（相对父），ILCombine 拼全。
            LPCITEMIDLIST parent =
                reinterpret_cast<LPCITEMIDLIST>(reinterpret_cast<BYTE*>(cida) + cida->aoffset[0]);
            for (UINT i = 1; i <= cida->cidl; ++i) {
                LPCITEMIDLIST child =
                    reinterpret_cast<LPCITEMIDLIST>(reinterpret_cast<BYTE*>(cida) +
                                                    cida->aoffset[i]);
                PIDLIST_ABSOLUTE full = ILCombine(parent, child);
                if (!full) continue;
                std::wstring p = PidlToPathOrNamespace(full);
                CoTaskMemFree(full);
                if (!p.empty()) paths.push_back(std::move(p));
            }
        }
        if (cida) GlobalUnlock(sm.hGlobal);
        ReleaseStgMedium(&sm);
        return paths;
    }

    // 回退：部分虚拟项不从 IDataObject 给 IDList，用 SHGetIDListFromObject。
    PIDLIST_ABSOLUTE pidlObj = nullptr;
    if (SUCCEEDED(SHGetIDListFromObject(pdo, &pidlObj)) && pidlObj) {
        std::wstring p = PidlToPathOrNamespace(pidlObj);
        CoTaskMemFree(pidlObj);
        if (!p.empty()) paths.push_back(std::move(p));
    }
    return paths;
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
        UpdatePreview(pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect &= (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        UpdatePreview(pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (owner_) owner_->ClearDropPreview();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pdo, DWORD, POINTL pt,
                                   DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        // Drop 的落点才是最终容器；不依赖 DragOver 的最后一次坐标。
        BookmarkDropSpot spot;
        if (owner_) {
            // 原生 IDropTarget 的 pt 已经是**屏幕坐标**（MFC 包装类才会转成客户区）。
            // 以前这里再 ClientToScreen 一次，落点被推到书签栏外，预览永远不出现，
            // 提交时 spot.valid=false，于是只剩“追加到末尾”的旧行为。
            POINT p{static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)};
            spot = owner_->DropSpotAt(p);
            owner_->ClearDropPreview();
            LogDbg(L"[bmk] OLE drop screen=%ld,%ld spot=%d idx=%d into=%d", p.x, p.y,
                   spot.valid ? 1 : 0, spot.index, spot.folderIndex);
        }
        const std::vector<std::wstring> paths = ExtractPaths(pdo);
        const int added = owner_ ? owner_->ApplyDroppedPaths(paths, spot) : 0;
        *pdwEffect = added > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        LogDbg(L"[bmk] 拖放（走命令总线）：成功 %d，跳过 %zu", added, paths.size() - added);
        return S_OK;
    }

    void Set(HWND h, BookmarkBar* owner) {
        hwnd_ = h;
        owner_ = owner;
    }

private:
    void UpdatePreview(POINTL pt) {
        if (!owner_) return;
        // 同 Drop：这里的 pt 已经是屏幕坐标，不能再 ClientToScreen。
        // PreviewDropAt = 落点预览 + 边缘自动滚动起停（外部拖文件夹拖到栏边缘
        // 也要像内部拖书签一样自动滚过去 —— 与 Chrome/Firefox 书签栏同款行为）。
        owner_->PreviewDropAt(POINT{static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)});
    }

    ULONG ref_ = 1;
    HWND hwnd_ = nullptr;
    BookmarkBar* owner_ = nullptr;
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
