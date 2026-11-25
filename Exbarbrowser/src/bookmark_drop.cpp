#include "bookmark_drop.h"

#include "bookmark_bar.h"
#include "commands.h"      // CommandBus：拖放也必须走命令（审计/撤销/既有语义同源）
#include "icon_resolve.h"
#include "json.h"
#include "log.h"

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
    // 一条拖入项 → 一条命令。
    //
    // 【为什么必须走 CommandBus，而不是直接 store.AddPath / store.MoveTo】
    // 拖放曾是唯一一条**绕开命令总线的写入路径**：它自己 AddPath/MoveTo/Save，
    // 于是同样一件事，从菜单做会被审计、会进撤销栈、会走 bmk.add.path 的
    // "已收藏则移动"语义；从拖放做则一条审计都没有，撤销栈也不记
    // （AddPath 拍快照是 2026-11-23 才补的，此前拖放还完全不进栈）。
    // 后果是"谁改了我的书签"在审计里查不到，而 AI 侧读到的 undoable=1 也兑现不了。
    //
    // 现在两种落点各对应一条既有命令：
    //   · 新路径（或给了目标分类）→ bmk.add.path {path, parentId}
    //     —— handler 内部已经实现"路径已存在且给了 parentId 就移动"这条语义，
    //        这里不需要再判一次（判两次必然有一天分叉）。
    //   · 已存在且拖到栏上（folderId 为空）→ 无事可做，如实计入 skipped。
    //
    // 返回值：1 = 新增/移动成功，0 = 无事可做或失败（原因已由命令写进审计）。
    int DropOne(const std::wstring& path, const std::wstring& folderId) {
        if (path.empty()) return 0;
        json::Value args = json::Value::MakeObject();
        // 显示名必须由**拖放侧**给：bmk.add.path 的默认名是 LeafOf（末段），
        // 对命名空间项（::{GUID}）会得到一串裸 GUID —— 拖"此电脑"进来，
        // 书签栏上就显示 "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"。
        // 原来的拖放直接调 store.AddPath(NameFor(p), ...)，靠 ico::DisplayName
        // 取真名。改成走命令时若不带 name，这个行为就静默退化了 ——
        // 所以 name 作为可选参数显式传进去（schema 里它本来就是可选的）。
        args.Set(L"name", json::Value::MakeStr(NameFor(path)));
        args.Set(L"path", json::Value::MakeStr(path));
        if (!folderId.empty()) args.Set(L"parentId", json::Value::MakeStr(folderId));
        return CommandBus::Get().Invoke(CmdId::BmkAdd, Args::FromJson(args), Actor::Drop()) ? 1
                                                                                           : 0;
    }

    void ExtractAndAdd(IDataObject* pdo, const std::wstring& folderId) {
        if (!pdo) return;
        int added = 0;
        int skipped = 0;

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
                for (UINT i = 1; i <= cida->cidl; ++i) {
                    LPCITEMIDLIST child =
                        reinterpret_cast<LPCITEMIDLIST>(reinterpret_cast<BYTE*>(cida) +
                                                        cida->aoffset[i]);
                    PIDLIST_ABSOLUTE full = ILCombine(parent, child);
                    if (!full) { ++skipped; continue; }
                    std::wstring p = PidlToPathOrNamespace(full);
                    CoTaskMemFree(full);
                    if (p.empty() || !DropOne(p, folderId)) { ++skipped; continue; }
                    ++added;
                }
            }
            if (cida) GlobalUnlock(sm.hGlobal);
            ReleaseStgMedium(&sm);
        } else {
            // 回退：部分虚拟项不从 IDataObject 给 IDList，用 SHGetIDListFromObject。
            PIDLIST_ABSOLUTE pidlObj = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(pdo, &pidlObj)) && pidlObj) {
                std::wstring p = PidlToPathOrNamespace(pidlObj);
                CoTaskMemFree(pidlObj);
                if (p.empty() || !DropOne(p, folderId)) ++skipped;
                else ++added;
            }
        }

        // 刷新只在真的改了之后做：命令 handler 已经 Save + Reload 过一次，
        // 这里补一次 ReloadExternal 是为了让**本栏**（拖放的宿主）也重排 ——
        // handler 刷的是 App 持有的同一实例，但拖放期间栏正处在拖拽态，
        // 显式再刷一次才能保证落点位置立刻正确。
        if (added) owner_->ReloadExternal();
        LogDbg(L"[bmk] 拖放（走命令总线）：成功 %d，跳过 %d", added, skipped);
    }

    static std::wstring NameFor(const std::wstring& path) {
        // 命名空间项的显示名统一由 ico::DisplayName 推导（此电脑/回收站/第三方 NSE）；
        // 文件系统路径仍取末段，避免把完整路径塞进书签栏按钮。
        if (nav::IsNamespace(path)) return ico::DisplayName(path);
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
