#include "shell_ops.h"
#include "str.h"

#include <propkey.h>

#include "path_util.h"
#include "text_file.h"
#include "view_stats.h"

namespace shellops {

ComPtr<IFolderView2> FolderView2(IShellView* view) {
    ComPtr<IFolderView2> fv2;
    // ComPtr::operator& 会先 Reset，所以这里传 &fv2 不会泄漏既有引用。
    if (view) view->QueryInterface(IID_IFolderView2, reinterpret_cast<void**>(&fv2));
    return fv2;
}

ComPtr<IShellFolder> ShellFolder(IFolderView2* fv2) {
    ComPtr<IShellFolder> folder;
    if (fv2) fv2->GetFolder(IID_IShellFolder, reinterpret_cast<void**>(&folder));
    return folder;
}
namespace {

// HRESULT 的十六进制文本（给"文件操作失败"这类回执用）。
// 为什么自己转：Windows 没有现成的 wchar_t 版本格式化函数，
// 而这些代码路径上不能引入 CRT 格式化的 locale 差异。
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

// 实现在本文件的公开区（见下方 FolderView2 / ShellFolder）；这里只放别名，
// 让本文件内既有调用点零改动。
ComPtr<IFolderView2> GetFv2(IShellView* view) { return FolderView2(view); }
ComPtr<IShellFolder> GetFolder(IFolderView2* fv2) { return ShellFolder(fv2); }

bool RunOpEx(IFileOperation* op, std::wstring* why);

// 执行 IFileOperation 并**同时**检查两件事：
//   1. PerformOperations 的 HRESULT
//   2. GetAnyOperationsAborted
//
// 只查 ① 是不够的：用户点取消、目标在过程中消失、部分失败，都会让
// PerformOperations 返回 S_OK 而 GetAnyOperationsAborted 为 TRUE。
// 原来只有 RenameTo 查了 ②，其余四处（删除/新建文件夹/转移/新建文本文件）
// 直接 return SUCCEEDED(...) —— 于是"用户取消了删除"在审计里被记成 ok=1，
// 调用方也以为文件已经没了。
bool RunOp(IFileOperation* op) {
    std::wstring why;
    return RunOpEx(op, &why);
}

// 同上，但把失败原因写进 why。
//
// 为什么单独留一个 Ex 版本：shell 的失败原因**只存在于 HRESULT 里**
// （"目标已存在" / "访问被拒绝" / "名称非法"），返回 bool 就等于把
// 三种完全不同的下一步压成同一个 false —— 调用方只能给出一句
// "重命名失败（名称非法或目标被占用）"，这句话对每一种原因都不算错，
// 但对使用者毫无用处（该换名？该关掉占用它的程序？还是权限问题？）。
bool RunOpEx(IFileOperation* op, std::wstring* why) {
    if (!op) {
        if (why) *why = L"文件操作对象创建失败";
        return false;
    }
    const HRESULT hr = op->PerformOperations();
    if (FAILED(hr)) {
        if (why) *why = L"文件操作失败（HRESULT=0x" + HexHr(hr) + L"）";
        LogDbg(L"[shellops] PerformOperations 失败 hr=0x%08X", static_cast<unsigned>(hr));
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

// FOF_RENAMEONCOLLISION 只会把名字改成 "<base> (2).txt" 这类形式。
// 回扫目录时必须只认这些名字，否则目录里任意较新的 .txt 都会被当成"刚建的"。
bool IsGeneratedTextName(const std::wstring& name, const std::wstring& base) {
    const std::wstring ext = L".txt";
    if (name.size() <= ext.size()) return false;
    if (name.compare(name.size() - ext.size(), ext.size(), ext) != 0) return false;
    const std::wstring stem = name.substr(0, name.size() - ext.size());
    if (stem == base) return true;
    if (stem.size() <= base.size() + 2) return false;          // 至少 " (n)"
    if (stem.compare(0, base.size(), base) != 0) return false; // 必须同源
    const size_t i = base.size();
    if (stem[i] != L' ' || stem[i + 1] != L'(') return false;
    const size_t close = stem.size() - 1;
    if (stem[close] != L')' || close <= i + 2) return false;   // 至少 1 位数字
    for (size_t k = i + 2; k < close; ++k)
        if (stem[k] < L'0' || stem[k] > L'9') return false;
    return true;
}

bool Invoke(IContextMenu* cm, HWND owner, const char* verb) {
    if (!cm) return false;
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = 0;
    info.hwnd = owner;
    info.lpVerb = verb;
    info.lpVerbW = nullptr;
    info.nShow = SW_SHOWNORMAL;
    HRESULT hr = cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    return SUCCEEDED(hr);
}

bool InvokeOnSelection(IShellView* view, HWND owner, const char* verb) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;

    // 显示的文件夹本身就是选中项的父文件夹（不要 SHBindToParent+BindToObject：
    // 那会拿到"选中项自己的文件夹"，再用兄弟项的 PIDL 去 GetUIObjectOf 就全错了）。
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return false;

    // 用 GetSelection 而不是 GetSelectionMarkedItem：Shift 多选时标记项只代表焦点项，
    // 重命名/属性会只作用一个；复选模式下甚至没有标记项 → 命令静默失败。
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return false;
    DWORD n = 0;
    sel->GetCount(&n);
    if (!n) return false;

    std::vector<PCUITEMID_CHILD> pidls;
    pidls.reserve(static_cast<size_t>(n));
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        PIDLIST_ABSOLUTE full = nullptr;
        if (FAILED(SHGetIDListFromObject(item.get(), &full)) || !full) continue;
        pidls.push_back(ILCloneChild(ILFindLastID(full)));
        CoTaskMemFree(full);
    }
    if (pidls.empty()) return false;

    bool ok = false;
    ComPtr<IContextMenu> cm;
    if (SUCCEEDED(folder->GetUIObjectOf(owner, static_cast<UINT>(pidls.size()), pidls.data(),
                                        IID_IContextMenu, nullptr, reinterpret_cast<void**>(&cm))) &&
        cm)
        ok = Invoke(cm.get(), owner, verb);
    for (auto* p : pidls) CoTaskMemFree(const_cast<PITEMID_CHILD>(p));
    return ok;
}

}  // namespace

namespace {

// 给 IDataObject 打上"这是剪切还是复制"的标记。
// CopySelection 与 CopyPaths 都要用 —— 抽出来免得两份实现漂移。
void MarkPreferredDropEffect(IDataObject* data, DWORD effect) {
    if (!data) return;
    FORMATETC fmt{};
    fmt.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    med.tymed = TYMED_HGLOBAL;
    med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!med.hGlobal) return;
    auto* p = static_cast<DWORD*>(GlobalLock(med.hGlobal));
    if (p) {
        *p = effect;
        GlobalUnlock(med.hGlobal);
    }
    // SetData 失败时 STGMEDIUM 不会被接管，必须自己释放（否则泄漏）。
    if (FAILED(data->SetData(&fmt, &med, TRUE))) ReleaseStgMedium(&med);
}

}  // namespace

bool CopySelection(IShellView* view, bool cut) {
    if (!view) return false;
    ComPtr<IDataObject> data;
    if (FAILED(view->GetItemObject(SVGIO_SELECTION, IID_IDataObject,
                                   reinterpret_cast<void**>(&data))) ||
        !data)
        return false;

    if (cut) MarkPreferredDropEffect(data.get(), DROPEFFECT_MOVE);
    HRESULT hr = OleSetClipboard(data.get());
    if (SUCCEEDED(hr)) OleFlushClipboard();
    return SUCCEEDED(hr);
}

bool CopyPaths(const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty()) return false;

    // 外部路径 → shell 能解析的形式。
    //
    // 为什么必须在这里归一化：SHCreateItemFromParsingName / SHParseDisplayName
    // 只认**反斜杠**，收到正斜杠直接返回 E_INVALIDARG(0x80070057)。
    // 而正斜杠在外部输入里完全合法（Win32 的 CreateFile 也接受，
    // AI 也天然会写 "C:/dir/file.txt"）。不归一化的后果是：
    //   fs.copy.toclip {"targets":["C:/a/b.txt"]} → 整批失败，
    //   报错却是"无法解析 C:/a/b.txt"，看不出真正原因是分隔符。
    //
    // 归一化之后，这一批路径就是后面**唯一**使用的形式 ——
    // 解析、比较、日志、撤销记录看到的都是它，不会出现"一处反斜杠一处正斜杠"。
    std::vector<std::wstring> sp;
    sp.reserve(paths.size());
    for (const auto& p : paths) sp.push_back(pathutil::ToShellPath(p));

    // 按路径造 IDataObject：用 SHCreateItemFromParsingName 拿 IShellItem，
    // 再交给 SHCreateDataObject —— 这是 shell 官方支持的"用指定项造 data object"。
    //
    // 为什么不自己拼 CF_HDROP：自己拼只能表达"文件名列表"，会丢掉
    // "这是来自哪个命名空间"的上下文（比如回收站里的项、库里的项），
    // 粘贴到某些目标会失败。让 shell 自己造，等价于用户在资源管理器里复制。
    ComPtr<IShellItemArray> arr;
    {
        std::vector<ComPtr<IShellItem>> items;
        items.reserve(sp.size());
        for (const auto& p : sp) {
            ComPtr<IShellItem> it;
            if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_IShellItem,
                                                   reinterpret_cast<void**>(&it))) ||
                !it) {
                // 一个都不许漏：漏了会造成"我复制了 5 个，实际只复制了 3 个"，
                // 而调用方以为全成了。宁可整批失败。
                LogDbg(L"[shell] CopyPaths: 无法解析 %s", p.c_str());
                return false;
            }
            items.push_back(std::move(it));
        }
        // SHCreateShellItemArrayFromShellItem 只接受单个；多个要用 IDataObject 的
        // 聚合路径，见下。
        if (items.size() == 1) {
            if (FAILED(SHCreateShellItemArrayFromShellItem(items[0].get(), IID_IShellItemArray,
                                                          reinterpret_cast<void**>(&arr))) ||
                !arr)
                return false;
        } else {
            // 多个：SHCreateDataObject 收的是"父目录的绝对 PIDL + 子 PIDL 数组"。
            // 走这条路而不是自己拼 CF_HDROP —— 后者只表达文件名列表，
            // 会丢掉"来自哪个命名空间"（回收站项、库项），粘贴到某些目标会失败。
            //
            // 【必须全部同父】SHELLIDLIST 的语义是"父 PIDL + **相对该父**的 child"。
            // 原实现只取 paths[0] 的父目录，其余路径的 ILFindLastID 被硬当成同一个
            // 父的子项 —— 跨目录时构造出的是**不存在的项**。而 FsCopy/FsCut 只校验
            // 每个路径存在，完全不校验同父，于是 AI 传
            // ["C:\\a\\x.txt","C:\\b\\y.txt"] 会得到"粘贴时要么失败要么粘错东西"，
            // 且命令返回成功。这比崩溃更难发现。
            //
            // 跨目录的副本得用 IDataObject 逐项聚合，那是另一套写法；在没实现它之前，
            // 明确拒绝并留日志，好过静默做错。
            const std::wstring parent0 = ParentOf(sp[0]);
            for (size_t i = 1; i < sp.size(); ++i) {
                const std::wstring pi = ParentOf(sp[i]);
                // CompareStringOrdinal + 忽略大小写：与 SamePath（本文件后部）同口径，
                // 这里不能用 _wcsicmp —— 它依赖 locale，土耳其语等区域会把 i/I 判错。
                if (CompareStringOrdinal(pi.c_str(), -1, parent0.c_str(), -1, TRUE) != CSTR_EQUAL) {
                    LogDbg(L"[shell] CopyPaths: 跨目录多选不支持（%s 与 %s 不同父）",
                           sp[0].c_str(), sp[i].c_str());
                    return false;
                }
            }

            PIDLIST_ABSOLUTE dirAbs = nullptr;
            SFGAOF attr = 0;
            if (FAILED(SHParseDisplayName(parent0.c_str(), nullptr, &dirAbs, 0, &attr)) ||
                !dirAbs)
                return false;

            std::vector<PCUITEMID_CHILD> children;
            children.reserve(sp.size());
            for (const auto& p : sp) {
                PIDLIST_ABSOLUTE abs = nullptr;
                if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &abs, 0, &attr)) || !abs) {
                    for (const auto* c : children) CoTaskMemFree(const_cast<ITEMIDLIST*>(c));
                    CoTaskMemFree(dirAbs);
                    return false;
                }
                // 取出最后一个 SHITEMID —— 它才是相对父目录的子项标识。
                PITEMID_CHILD child = ILFindLastID(abs);
                PITEMID_CHILD copy = ILCloneChild(child);
                CoTaskMemFree(abs);
                if (!copy) {
                    for (const auto* c : children) CoTaskMemFree(const_cast<ITEMIDLIST*>(c));
                    CoTaskMemFree(dirAbs);
                    return false;
                }
                children.push_back(copy);   // 所有权仍在自己手上，下面统一释放
            }

            ComPtr<IDataObject> data;
            // 用 CIDLData_CreateFromIDArray 而不是 SHCreateDataObject：
            // 后者被 NTDDI_VERSION >= 0x06000000 包着，某些编译配置下不可见；
            // 前者无条件可用，参数也正好是"父目录 + 相对子 PIDL 数组"。
            HRESULT hr = CIDLData_CreateFromIDArray(
                dirAbs, static_cast<UINT>(children.size()),
                reinterpret_cast<PCUIDLIST_RELATIVE_ARRAY>(children.data()), &data);
            for (const auto* c : children) CoTaskMemFree(const_cast<ITEMIDLIST*>(c));
            CoTaskMemFree(dirAbs);
            if (FAILED(hr) || !data) return false;

            if (cut) MarkPreferredDropEffect(data.get(), DROPEFFECT_MOVE);
            hr = OleSetClipboard(data.get());
            if (SUCCEEDED(hr)) OleFlushClipboard();
            return SUCCEEDED(hr);
        }
    }

    ComPtr<IDataObject> data;
    if (FAILED(arr->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject,
                                  reinterpret_cast<void**>(&data))) ||
        !data)
        return false;

    if (cut) MarkPreferredDropEffect(data.get(), DROPEFFECT_MOVE);
    const HRESULT hr = OleSetClipboard(data.get());
    if (SUCCEEDED(hr)) OleFlushClipboard();
    return SUCCEEDED(hr);
}

bool Paste(IShellView* view, HWND owner) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return false;
    ComPtr<IContextMenu> cm;
    if (FAILED(folder->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(&cm))) ||
        !cm)
        return false;
    return Invoke(cm.get(), owner, "paste");
}

namespace {

// DeleteSelection 与 DeleteSelectionPermanently 的唯一差别是 FOF_ALLOWUNDO。
// 抽成一份实现，避免"回收站版"和"永久版"日后在选中集/错误处理上分叉。
bool DeleteSelectionImpl(IShellView* view, HWND owner, bool permanent) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return false;
    DWORD n = 0;
    sel->GetCount(&n);
    if (!n) return false;

    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    DWORD flags = FOF_NOCONFIRMMKDIR | FOF_WANTNUKEWARNING;
    // 普通 Delete 进回收站，系统撤销栈能退；Shift+Delete 不带这一位。
    if (!permanent) flags |= FOF_ALLOWUNDO;
    op->SetOperationFlags(flags);
    op->SetOwnerWindow(owner);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        op->DeleteItem(item.get(), nullptr);
    }
    return RunOp(op.get());   // 同时查 GetAnyOperationsAborted
}

}  // namespace

bool DeleteSelection(IShellView* view, HWND owner) {
    return DeleteSelectionImpl(view, owner, false);
}

bool DeleteSelectionPermanently(IShellView* view, HWND owner) {
    return DeleteSelectionImpl(view, owner, true);
}

bool InvokeSelectionVerb(IShellView* view, HWND owner, const char* verb) {
    return InvokeOnSelection(view, owner, verb);
}

bool InvokeFolderVerb(IShellView* view, HWND owner, const char* verb) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return false;
    ComPtr<IContextMenu> cm;
    if (FAILED(folder->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(&cm))) ||
        !cm)
        return false;
    return Invoke(cm.get(), owner, verb);
}

namespace {

// "新建文件夹"/"新建文本文档"的名字要用系统本地化串：英文系统上 explorer 显示的是
// "New folder"，硬编码中文会显得格格不入。取 shell32 的间接字符串，失败再退回中文。
// "新建文件夹"的名字用 shell32 的间接字符串（英文系统返回 "New folder"）。
// 硬编码中文在英文系统上很突兀。
std::wstring LocalizedNewFolderName() {
    wchar_t out[128] = L"";
    if (SUCCEEDED(SHLoadIndirectString(L"@%SystemRoot%\\system32\\shell32.dll,-30396", out,
                                       _countof(out), nullptr)) &&
        out[0])
        return out;
    return L"新建文件夹";
}

// "新建文本文档"的默认名不是资源 ID，而是 ".txt\ShellNew" 里的 ItemName
// （指向 notepad.exe,-470 这类间接字符串）。以前猜 30397 拿到的是"新快捷方式"。
// 没有 ItemName 时（用 NullFile 的扩展名）就退回本地化的 "Text Document"。
std::wstring LocalizedNewTextName() {
    wchar_t item[512] = L"";
    DWORD cb = sizeof(item) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L".txt\\ShellNew", L"ItemName", RRF_RT_REG_SZ, nullptr,
                     item, &cb) == ERROR_SUCCESS &&
        item[0]) {
        wchar_t out[256] = L"";
        if (SUCCEEDED(SHLoadIndirectString(item, out, _countof(out), nullptr)) && out[0])
            return out;
    }
    // txtfile 的默认值（英文 "Text Document"，中文 "文本文档"）
    wchar_t doc[256] = L"";
    cb = sizeof(doc) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L".txt", nullptr, RRF_RT_REG_SZ, nullptr, doc, &cb) ==
            ERROR_SUCCESS &&
        doc[0]) {
        wchar_t keyPath[256] = L"";
        _snwprintf_s(keyPath, _countof(keyPath), _TRUNCATE, L"%ls", doc);
        wchar_t localized[256] = L"";
        cb = sizeof(localized) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CLASSES_ROOT, keyPath, L"FriendlyTypeName", RRF_RT_REG_SZ, nullptr,
                         localized, &cb) == ERROR_SUCCESS &&
            localized[0]) {
            wchar_t out2[256] = L"";
            if (SUCCEEDED(SHLoadIndirectString(localized, out2, _countof(out2), nullptr)) &&
                out2[0])
                return out2;
        }
        if (str::IEquals(doc, L"txtfile")) return L"新建文本文档";
    }
    return L"新建文本文档";
}

}  // namespace

bool NewFolder(IShellView* view, HWND owner) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellItem2> folder;
    if (FAILED(fv2->GetFolder(IID_IShellItem2, reinterpret_cast<void**>(&folder))) || !folder)
        return false;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    op->SetOwnerWindow(owner);
    // FOF_RENAMEONCOLLISION 是 explorer 的默认语义：重名时自动 "新建文件夹 (2)"，
    // 少了它会静默失败或弹冲突框（原来第二次新建文件夹必失败）。
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION);
    const std::wstring folderName = LocalizedNewFolderName();
    op->NewItem(folder.get(), FILE_ATTRIBUTE_DIRECTORY, folderName.c_str(), nullptr, nullptr);
    return RunOp(op.get());   // 同时查 GetAnyOperationsAborted
}

bool TransferToFolder(IShellView* view, HWND owner, const std::wstring& target, bool move) {
    if (!view || target.empty()) return false;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return false;
    DWORD n = 0;
    sel->GetCount(&n);
    if (!n) return false;

    ComPtr<IShellItem> dest;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_IShellItem,
                                            reinterpret_cast<void**>(&dest))) ||
        !dest)
        return false;

    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION);
    HRESULT hr = move ? op->MoveItems(sel.get(), dest.get()) : op->CopyItems(sel.get(), dest.get());
    if (FAILED(hr)) return false;
    return RunOp(op.get());   // 同时查 GetAnyOperationsAborted
}

bool NewTextFile(IShellView* view, HWND owner, std::wstring* created) {
    std::wstring dir = ViewFolderPath(view);
    if (dir.empty()) return false;

    // 优先 IFileOperation::NewItem：与"新建文件夹"同一条链路，可以 Ctrl+Z 撤销、
    // 冲突由 shell 按 FOF_RENAMEONCOLLISION 处理。以前裸 CreateFileW 建出来的文件
    // 不在撤销栈里，用户误建只能自己删。
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    ComPtr<IShellItem2> folder;
    if (fv2 && SUCCEEDED(fv2->GetFolder(IID_IShellItem2, reinterpret_cast<void**>(&folder))) &&
        folder) {
        ComPtr<IFileOperation> op;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                       reinterpret_cast<void**>(&op))) &&
            op) {
            op->SetOwnerWindow(owner);
            op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION);
            // 名字只算一次：LocalizedNewTextName 每次要读 2-3 次注册表，
            // 原来在这里调了 3 次（NewItem 一次、回填 created 两次）。
            const std::wstring baseName = LocalizedNewTextName();
            if (SUCCEEDED(op->NewItem(folder.get(), FILE_ATTRIBUTE_NORMAL,
                                      (baseName + L".txt").c_str(), nullptr, nullptr)) &&
                RunOp(op.get())) {
                // 【不能手拼 created】上面带了 FOF_RENAMEONCOLLISION，同目录已有同名
                // 文件时实际创建的是"新建文本文档 (2).txt"，而手拼回的是不带后缀的
                // 那份 —— 这个出参的契约是"告诉我建了哪个文件"，回错路径比不回更糟。
                // 改成创建后按目录实际扫描：只认 baseName 与 shell 的冲突后缀
                // "<base> (n).txt"；目录里别的新 .txt 不能被认成刚建的。
                if (created) {
                    // 先清空：回扫没命中时宁可交回空串，也不能把手拼的 baseName
                    // 当成实际创建结果（那正是这条修复要消灭的错误）。
                    created->clear();
                    WIN32_FIND_DATAW fd{};
                    FILETIME newest{};
                    HANDLE hf = FindFirstFileW(tf::ToExtended(tf::Join(dir, L"*.txt")).c_str(), &fd);
                    if (hf != INVALID_HANDLE_VALUE) {
                        do {
                            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                            const std::wstring candName = fd.cFileName;
                            if (!IsGeneratedTextName(candName, baseName)) continue;
                            const std::wstring cand = tf::Join(dir, candName);
                            if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
                                newest = fd.ftLastWriteTime;
                                *created = cand;
                            }
                        } while (FindNextFileW(hf, &fd));
                        FindClose(hf);
                    }
                }
                return true;
            }
        }
    }

    // 回退：某些命名空间视图不支持 NewItem，仍用 CreateFileW 保证功能可用。
    for (int i = 0; i < 1000; ++i) {
        // 原来缓冲只有 64 wchar：目录稍长就会被截断，文件建到剪断后的怪路径上。
        std::wstring name = dir + L"\\" + LocalizedNewTextName();
        if (i > 0) name += L" (" + std::to_wstring(i) + L")";
        name += L".txt";
        // ToExtended：新建的目标目录可能在深路径下（shell 的 NewItem 已处理，
        // 这条回退路径也必须处理，否则两个分支行为不一致）。
        HANDLE h = CreateFileW(tf::ToExtended(name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            if (created) *created = name;
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    return false;
}

// 选中项里恰好只有文件夹时返回其路径（CmdOpen 用它实现"文件夹在当前标签进入"）。
bool FirstFolderSelection(IShellView* view, std::wstring* path) {
    if (!path) return false;
    path->clear();
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return false;
    DWORD n = 0;
    sel->GetCount(&n);
    if (n != 1) return false;
    ComPtr<IShellItem> item;
    if (FAILED(sel->GetItemAt(0, &item)) || !item) return false;
    SFGAOF attr = SFGAO_FOLDER;
    if (FAILED(item->GetAttributes(SFGAO_FOLDER, &attr)) || !(attr & SFGAO_FOLDER)) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &p)) || !p) return false;
    *path = p;
    CoTaskMemFree(p);
    return !path->empty();
}

bool SelectAll(IShellView* view) {
    // IShellView::SelectItem(nullptr) 是"全选"的旧写法，在 IFolderView2 视图上不生效。
    // 逐项 SelectItem，去掉 DESELECTOTHERS。
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return false;
    bool any = false;
    for (int i = 0; i < count; ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        if (SUCCEEDED(view->SelectItem(pidl, SVSI_SELECT))) any = true;
        CoTaskMemFree(pidl);
    }
    return any;
}

bool SelectIndex(IShellView* view, int index) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2 || index < 0) return false;
    PITEMID_CHILD pidl = nullptr;
    if (FAILED(fv2->Item(index, &pidl)) || !pidl) return false;
    HRESULT hr = view->SelectItem(pidl, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE |
                                            SVSI_FOCUSED);
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr);
}

bool SetSort(IShellView* view, int mode, int ascending) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    const PROPERTYKEY* key = &PKEY_ItemNameDisplay;
    switch (mode) {
    case 1: key = &PKEY_DateModified; break;
    case 2: key = &PKEY_ItemTypeText; break;
    case 3: key = &PKEY_Size; break;
    default: break;
    }
    // 方向：
    //   ascending == 0/1 → 用调用方给的方向（启动恢复用，必须可指定）
    //   ascending == -1  → 与 explorer 一致：对同一列再次排序 = 反向
    // 不区分这两种语义的话，"启动时恢复上次的降序"就无法表达 —— 只会被当成
    // "再点一次"，反而把方向翻转回去。
    int dir = 1;
    if (ascending >= 0) {
        dir = ascending ? 1 : -1;
    } else {
        int cols = 0;
        if (SUCCEEDED(fv2->GetSortColumnCount(&cols)) && cols > 0) {
            cols = std::min(cols, 8);
            std::vector<SORTCOLUMN> cur(static_cast<size_t>(cols));
            if (SUCCEEDED(fv2->GetSortColumns(cur.data(), cols)) &&
                IsEqualGUID(cur[0].propkey.fmtid, key->fmtid) && cur[0].propkey.pid == key->pid &&
                cur[0].direction > 0)
                dir = -1;
        }
    }
    SORTCOLUMN col{};
    col.propkey = *key;
    col.direction = dir;
    // 不再调 view->Refresh()：SetSortColumns 本身就会让视图重排，
    // 额外 Refresh 在大目录上等于重新枚举一遍（实测几百 ms 卡顿）。
    return SUCCEEDED(fv2->SetSortColumns(&col, 1));
}

bool GetSort(IShellView* view, int* mode, bool* ascending) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    int cols = 0;
    if (FAILED(fv2->GetSortColumnCount(&cols)) || cols <= 0) return false;
    cols = std::min(cols, 8);
    std::vector<SORTCOLUMN> cur(static_cast<size_t>(cols));
    int got = cols;
    if (FAILED(fv2->GetSortColumns(cur.data(), got)) || got <= 0) return false;

    // 只认本工程能表达的四列；其它列返回 false，让调用方保留原配置。
    const PROPERTYKEY& k = cur[0].propkey;
    int m = -1;
    if (IsEqualGUID(k.fmtid, PKEY_ItemNameDisplay.fmtid) && k.pid == PKEY_ItemNameDisplay.pid)
        m = 0;
    else if (IsEqualGUID(k.fmtid, PKEY_DateModified.fmtid) && k.pid == PKEY_DateModified.pid)
        m = 1;
    else if (IsEqualGUID(k.fmtid, PKEY_ItemTypeText.fmtid) && k.pid == PKEY_ItemTypeText.pid)
        m = 2;
    else if (IsEqualGUID(k.fmtid, PKEY_Size.fmtid) && k.pid == PKEY_Size.pid)
        m = 3;
    if (m < 0) return false;

    if (mode) *mode = m;
    if (ascending) *ascending = cur[0].direction >= 0;
    return true;
}

bool SetGroup(IShellView* view, int mode) {
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    HRESULT hr = S_OK;
    if (mode == 0) {
        PROPERTYKEY none{};
        hr = fv2->SetGroupBy(none, FALSE);
    } else {
        const PROPERTYKEY* key = &PKEY_ItemTypeText;
        if (mode == 2) key = &PKEY_DateModified;
        if (mode == 3) key = &PKEY_Size;
        hr = fv2->SetGroupBy(*key, TRUE);
    }
    return SUCCEEDED(hr);
}

namespace {

DWORD HiddenFlags() { return SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN | SSF_SHOWEXTENSIONS; }

}  // namespace

bool ToggleHidden(IShellView* view, int which, HWND owner) {
    (void)owner;
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, HiddenFlags(), FALSE);
    if (which == 0)
        ss.fShowAllObjects = ss.fShowAllObjects ? FALSE : TRUE;
    else if (which == 1)
        ss.fShowSuperHidden = ss.fShowSuperHidden ? FALSE : TRUE;
    else
        ss.fShowExtensions = ss.fShowExtensions ? FALSE : TRUE;
    SHGetSetSettings(&ss, HiddenFlags(), TRUE);
    // SHCNE_ASSOCCHANGED 会让整个 shell 重建关联/图标缓存（系统级抖动）。
    // 这里只影响"当前目录显示哪些项"，用 UPDATEDIR 即可。
    const std::wstring dir = ViewFolderPath(view);
    if (!dir.empty())
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, dir.c_str(), nullptr);
    if (view) view->Refresh();
    return true;
}

int CycleHiddenState(IShellView* view, HWND owner) {
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, HiddenFlags(), FALSE);
    int state = 0;  // 0 全显 / 1 隐系统 / 2 隐全部
    if (!ss.fShowAllObjects)
        state = 2;
    else if (!ss.fShowSuperHidden)
        state = 1;
    int next = (state + 1) % 3;
    ss.fShowAllObjects = next == 2 ? FALSE : TRUE;
    ss.fShowSuperHidden = next == 0 ? TRUE : FALSE;
    SHGetSetSettings(&ss, HiddenFlags(), TRUE);
    const std::wstring dir = ViewFolderPath(view);
    if (!dir.empty())
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, dir.c_str(), nullptr);
    (void)owner;
    if (view) view->Refresh();
    return next;
}

HiddenFlagsState GetHiddenFlags() {
    // 与 ToggleHidden / CycleHiddenState **同一个掩码**（HiddenFlags 在上面，
    // 同一 TU 内共用）。掩码不一致就会出现"读到的是另一个开关"。
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, HiddenFlags(), FALSE);
    HiddenFlagsState st;
    st.showAllObjects = ss.fShowAllObjects != FALSE;
    st.showSuperHidden = ss.fShowSuperHidden != FALSE;
    st.showExtensions = ss.fShowExtensions != FALSE;
    // 三态口径与 CycleHiddenState 的编码完全一致（否则读到的三态与写出的三态
    // 不是同一个刻度，断言 view.cycle.hidden=$changed 就会误判）。
    st.triState = !st.showAllObjects ? 2 : (!st.showSuperHidden ? 1 : 0);
    return st;
}

// ---------------------------------------------------------------- 显式目标

namespace {

// 取某个子项（PITEMID_CHILD）的完整文件系统路径。
// 非文件系统项（命名空间扩展、搜索结果里的虚拟项）返回 false —— 这类项
// 没有稳定路径，按路径定位本来就不适用，调用方应改用索引或拒绝。
bool ItemFullPath(IShellFolder* folder, PCUITEMID_CHILD pidl, std::wstring* out) {
    if (!folder || !pidl || !out) return false;
    STRRET sr{};
    if (FAILED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr))) return false;

    // 【不要用固定栈缓冲】原来这里是 `wchar_t buf[MAX_PATH*2]`：
    // 路径超过 519 字符时 StrRetToBufW 返回 STRSAFE_E_INSUFFICIENT_BUFFER，
    // 函数返回 false，而调用方把 false 当成"这不是文件系统项" ——
    // SelectByPath / SelectByPaths / StatItem / RenameTo 对深路径项全部静默失败。
    // nav_path.h 的注释里已经记过这条教训（"不用 MAX_PATH 栈缓冲"），这里又犯了一次。
    //
    // 改成让 shell 自己分配（StrRetToStrW），再转 std::wstring。
    PWSTR pw = nullptr;
    if (FAILED(StrRetToStrW(&sr, pidl, &pw)) || !pw) return false;
    std::wstring path(pw);
    CoTaskMemFree(pw);   // shell 分配的所有权还给它

    // ::{GUID} / shell: 这类命名空间路径没有文件系统语义，不适合当"目标"。
    if (path.empty() || path[0] == L':') return false;
    *out = std::move(path);
    return true;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    // 大小写不敏感比较：Windows 路径语义。用 CompareStringOrdinal 而不是 _wcsicmp，
    // 后者依赖当前 locale，在土耳其语等区域会错。
    //
    // 先统一分隔符：Win32 文件路径里 '/' 与 '\\' 等价（Shell 解析、CreateFile 都接受），
    // 但序数比较不认。fs.* 的路径来自外部（AI / 脚本 / 命令行），传 "C:/x/a.txt"
    // 而视图回读是 "C:\x\a.txt" 时，全部 fs.* 会判成"目标不在当前视图"并报
    // "命令执行失败"（2026-11-21 结构化测试实测：rename/copy/move 三条全挂）。
    // 在这里收口，比在每个 handler 各写一遍规范化可靠。
    auto slash = [](const std::wstring& s) {
        std::wstring out = s;
        for (auto& c : out)
            if (c == L'/') c = L'\\';
        return out;
    };
    const std::wstring na = slash(a);
    const std::wstring nb2 = slash(b);
    return CompareStringOrdinal(na.c_str(), -1, nb2.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

bool SelectNone(IShellView* view) {
    if (!view) return false;
    // SelectItem(nullptr) 在 IFolderView2 视图上是"全选"，不是"清空"。
    // 必须逐个取消选中。
    //
    // 注意索引空间：ItemCount(SVGIO_SELECTION) 给的是"选中项个数"，而 Item(i)
    // 的 i 是**视图全局索引** —— 两者不能混用。若第 5、9 项被选中，
    // 混用会去取消第 0、1 项（取消错的项）。正确做法是用 Items() 直接
    // 拿到选中项那组 PIDL，不必自己按下标去够。
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;

    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->Items(SVGIO_SELECTION, IID_IShellItemArray,
                          reinterpret_cast<void**>(&sel))) ||
        !sel)
        return true;   // 没有选中项，已是目标状态

    DWORD n = 0;
    sel->GetCount(&n);
    if (n == 0) return true;

    bool any = false;
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        // 从 IShellItem 取回 PIDL 才能用 SelectItem 取消选中。
        ComPtr<IParentAndItem> pai;
        if (FAILED(item->QueryInterface(IID_IParentAndItem, reinterpret_cast<void**>(&pai))) ||
            !pai)
            continue;
        PIDLIST_ABSOLUTE abs = nullptr;
        if (FAILED(pai->GetParentAndItem(nullptr, nullptr, &abs)) || !abs) continue;
        // 视图里的 SelectItem 只认子 PIDL（最后一个 SHITEMID）。
        PCUITEMID_CHILD child = ILFindLastID(abs);
        if (child && SUCCEEDED(view->SelectItem(child, SVSI_DESELECT))) any = true;
        CoTaskMemFree(abs);
    }
    return any;
}

bool SelectByPath(IShellView* view, const std::wstring& fullPath) {
    if (!view || fullPath.empty()) return false;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return false;

    // 这里用 SVGIO_ALLVIEW 配 Item(i) 是**正确**的：同一个索引空间
    // （视图里第 i 项）。下面 SelectNone 不能用同样的写法，因为那里要遍历
    // "选中项"，不是"所有项"。
    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return false;

    for (int i = 0; i < count; ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        std::wstring p;
        const bool got = ItemFullPath(folder.get(), pidl, &p);
        if (got && SamePath(p, fullPath)) {
            // DESELECTOTHERS 保证"就选这一个"——调用方要的是精确目标，
            // 不是"在用户已有选中里再加一个"。
            HRESULT hr = view->SelectItem(pidl, SVSI_SELECT | SVSI_DESELECTOTHERS |
                                                    SVSI_ENSUREVISIBLE | SVSI_FOCUSED);
            CoTaskMemFree(pidl);
            return SUCCEEDED(hr);
        }
        CoTaskMemFree(pidl);
    }
    return false;   // 不在当前视图
}

bool SelectedSinglePath(IShellView* view, std::wstring* fullPath) {
    if (!view || !fullPath) return false;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;

    // 同 SelectNone：用 Items(SVGIO_SELECTION) 拿选中项，而不是拿
    // "选中个数"当索引去 Item()。
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->Items(SVGIO_SELECTION, IID_IShellItemArray,
                          reinterpret_cast<void**>(&sel))) ||
        !sel)
        return false;

    DWORD n = 0;
    sel->GetCount(&n);
    if (n != 1) return false;   // 恰好一个才谈得上"唯一选中项"

    ComPtr<IShellItem> item;
    if (FAILED(sel->GetItemAt(0, &item)) || !item) return false;

    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    *fullPath = p;
    CoTaskMemFree(p);
    return true;
}

int SelectByPaths(IShellView* view, const std::vector<std::wstring>& paths) {
    if (!view || paths.empty()) return 0;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return 0;
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return 0;

    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return 0;

    // 先建"剩余待找集合"，避免 O(视图项数 × 目标数) 的重复比较 ——
    // 批量操作的目标可能上百个，而视图里可能有上万个项目。
    std::vector<std::wstring> remaining = paths;
    int hits = 0;
    bool first = true;

    for (int i = 0; i < count && !remaining.empty(); ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        std::wstring p;
        if (!ItemFullPath(folder.get(), pidl, &p)) {
            CoTaskMemFree(pidl);
            continue;
        }
        for (size_t k = 0; k < remaining.size(); ++k) {
            if (!SamePath(remaining[k], p)) continue;
            // 第一个用 DESELECTOTHERS 清掉用户原有选中，后续叠加。
            SVSIF flags = SVSI_SELECT | SVSI_ENSUREVISIBLE;
            if (first) flags |= SVSI_DESELECTOTHERS;
            if (SUCCEEDED(view->SelectItem(pidl, flags))) {
                ++hits;
                first = false;
            }
            remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(k));
            break;
        }
        CoTaskMemFree(pidl);
    }
    return hits;
}

int SelectWhere(IShellView* view, const std::function<bool(const DirEntry&)>& pred,
                SelectMode mode) {
    if (!view || !pred) return 0;
    DirListing list;
    if (!ListView(view, 0, &list)) return 0;   // 没有视图
    if (list.entries.empty()) return 0;

    // 先筛出命中路径，再交给 SelectByPaths —— 枚举/选中各只有一份实现。
    //
    // 不在这里逐个 SelectItem 的原因：那会绕开 SelectByPaths 已经处理过的
    // "第一项带 DESELECTOTHERS" 语义，Replace 模式下反选的结果会变成
    // "旧选中 + 新选中"的并集（用户看到的是"反选后什么都在选中的状态里"）。
    std::vector<std::wstring> hits;
    hits.reserve(list.entries.size());
    for (const auto& e : list.entries) {
        if (e.path.empty()) continue;   // 命名空间项无完整路径，SelectByPaths 也匹配不到
        if (pred(e)) hits.push_back(e.path);
    }
    if (hits.empty()) {
        // "一个都不满足"时清空选中：反选的语义是"反过来选"，结果为空是合法答案，
        // 保留旧选中会让用户以为命令没执行。Add 模式则什么都不做（不该破坏现状）。
        if (mode == SelectMode::Replace) SelectNone(view);
        return 0;
    }
    if (mode == SelectMode::Add) {
        // 追加：先不清空。SelectByPaths 的 first 分支会 DESELECTOTHERS，
        // 所以追加要单独处理 —— 直接逐项 SVSI_SELECT（不清别人）。
        ComPtr<IFolderView2> fv2 = GetFv2(view);
        if (!fv2) return 0;
        ComPtr<IShellFolder> folder = GetFolder(fv2.get());
        if (!folder) return 0;
        int count = 0;
        if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return 0;
        int hitsN = 0;
        for (int i = 0; i < count; ++i) {
            PITEMID_CHILD pidl = nullptr;
            if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
            std::wstring p;
            const bool got = ItemFullPath(folder.get(), pidl, &p);
            if (got) {
                for (const auto& h : hits) {
                    if (!SamePath(h, p)) continue;
                    if (SUCCEEDED(view->SelectItem(pidl, SVSI_SELECT))) ++hitsN;
                    break;
                }
            }
            CoTaskMemFree(pidl);
        }
        return hitsN;
    }
    return SelectByPaths(view, hits);
}

int SelectByPattern(IShellView* view, const std::wstring& patterns, const PatternOptions& opt) {
    if (patterns.empty()) return 0;

    // 拆成多个模式（分号分隔）。顺手去掉两端空白 ——
    // 用户从别处粘 "*.txt; *.md" 时带空格很常见，不 Trim 就会有一个模式永远匹配不上。
    std::vector<std::wstring> pats;
    size_t start = 0;
    while (start <= patterns.size()) {
        const size_t semi = patterns.find(L';', start);
        const std::wstring one =
            str::Trim(patterns.substr(start, semi == std::wstring::npos ? std::wstring::npos
                                                                      : semi - start));
        if (!one.empty()) pats.push_back(one);
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
    if (pats.empty()) return 0;

    // 匹配语义交给 Shlwapi 的 PathMatchSpecExW：它就是资源管理器里"*.txt"那套
    // 通配（`*` / `?`，分号天然支持多模式）。**不自创语法** —— 用户对此已有肌肉记忆。
    // 大小写：默认不敏感（与资源管理器一致）。mingw 的 shlwapi.h 只声明了
    // PMSF_NORMAL/MULTIPLE/DONT_STRIP_SPACES，没有 PMSF_CASESENSITIVE ——
    // 那个值（0x80000000）在 Win32 文档里是公开的，这里按文档写常量，
    // 而不是"为了能编过就悄悄丢掉大小写选项"（那会让 caseSensitive 参数变成装饰）。
    constexpr DWORD kPmsfCaseSensitive = 0x80000000;
    const DWORD kFlags = opt.caseSensitive ? kPmsfCaseSensitive : PMSF_NORMAL;
    return SelectWhere(
        view,
        [&](const DirEntry& e) {
            if (!opt.matchFolders && e.isFolder) return false;
            if (e.name.empty()) return false;
            // 【必须用磁盘上的名字，不能用显示名】DirEntry::name 是视图显示名，
            // 关掉"显示扩展名"时它是 "a_r" 而不是 "a_r.txt" —— 拿它匹配 "*.txt"
            // 永远匹配不上（2026-11-21 实测：修掉 S_FALSE 之后变成"一个都选不中"）。
            const wchar_t* leaf = e.path.empty() ? nullptr : PathFindFileNameW(e.path.c_str());
            const std::wstring target = (leaf && *leaf) ? std::wstring(leaf) : e.name;
            for (const auto& p : pats) {
                // 【必须写 == S_OK】PathMatchSpecExW 用 S_OK / S_FALSE 表示匹配 / 不匹配，
                // 而 SUCCEEDED(S_FALSE) 为真 —— 写成 SUCCEEDED(...) 等于"全都匹配"。
                if (PathMatchSpecExW(target.c_str(), p.c_str(), kFlags) == S_OK) return true;
            }
            return false;
        },
        SelectMode::Replace);
}

std::vector<std::wstring> SelectedPaths(IShellView* view) {
    std::vector<std::wstring> out;
    if (!view) return out;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return out;

    // 用 Items(SVGIO_SELECTION) 拿选中项，而不是拿"选中个数"当索引去 Item()。
    // 后者在"选中项不连续"时会取到错误项（与 SelectNone / SelectedSinglePath 同一条坑）。
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->Items(SVGIO_SELECTION, IID_IShellItemArray, reinterpret_cast<void**>(&sel))) ||
        !sel)
        return out;
    DWORD n = 0;
    sel->GetCount(&n);
    out.reserve(n);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            out.emplace_back(p);
            CoTaskMemFree(p);
        }
    }
    return out;
}

bool Undo(IShellView* view) {
    if (!view) return false;

    // 【怎么撤】向视图的背景上下文菜单要 "undo" 谓词并调用它。
    //
    // 为什么不用 FCIDM_SHVIEW_UNDO(0x701B) + WM_COMMAND：
    //   · 那个常量在 Windows 头文件里根本不存在（只有 Wine 收录了）；
    //   · 它紧邻 FCIDM_SHVIEW_DELETE(0x7011)，猜错一位就是**删文件** ——
    //     一个"撤销"键变成删除，没有任何比这更糟的失败方式。
    //
    // 为什么不是自建撤销栈：所有落盘操作都带 FOF_ALLOWUNDO，已经进了系统
    // 撤销栈（资源管理器 Ctrl+Z 撤的就是它）。自建一份会和系统栈错位。
    //
    // 走 IContextMenu 是全部文档化的一步：
    //   1. GetItemObject(SVGIO_BACKGROUND, IID_IContextMenu) 拿背景菜单；
    //   2. 用 QueryContextMenu 填充（shell 只在"确实有可撤销项"时才加 Undo 行）；
    //   3. 逐项 GetCommandString(GCS_VERBW) 找出规范动词名；
    //   4. 用该动词 InvokeCommand —— 不自己构造 idCmd。
    // 找不到 undo 行 = 没有可撤销的操作，如实返回 false（调用方响一声）。
    ComPtr<IContextMenu> cm;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IContextMenu,
                                   reinterpret_cast<void**>(&cm))) ||
        !cm) {
        LogDbg(L"[undo] GetItemObject(IContextMenu) 失败");
        return false;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;

    // 命令 id 段用 1..0x6FFF（在 FCIDM_SHVIEWFIRST..LAST 之内，避免与全局菜单冲突）。
    const int n = cm->QueryContextMenu(menu, 0, 1, 0x6FFF, CMF_NORMAL | CMF_NODEFAULT);
    bool done = false;
    if (n > 0) {
        const int count = GetMenuItemCount(menu);
        for (int i = 0; i < count && !done; ++i) {
            const UINT id = GetMenuItemID(menu, i);
            if (id == static_cast<UINT>(-1)) continue;   // 分隔线/子菜单
            // 【不要用 GetCommandString 逐项问动词名】
            //
            // 实测踩出来的坑：GCS_VERBW 会为该菜单项**懒加载对应的 shell 扩展**。
            // 第三方扩展（云盘客户端/右键增强）一旦在它的处理里阻塞，这里就永久
            // 挂住 —— 而本函数运行在 UI 线程（ai::PumpRequests 由定时器驱动），
            // 后果是整个消息泵卡死：界面不再重绘，管道后续请求全部返回空。
            // 实测日志：QueryContextMenu 正常返回 122 项，循环第一次 GetCommandString
            // 就再没回来。
            //
            // 改用菜单项自身的**文本**：shell 给 undo 行加的 caption 是本地化的
            // "撤销 <操作名>" / "Undo <op>"。读菜单文本是纯菜单操作，不碰扩展。
            wchar_t text[160] = L"";
            if (!GetMenuStringW(menu, id, text, static_cast<int>(_countof(text)), MF_BYCOMMAND))
                continue;
            // 前缀匹配（而不是"包含"）：shell 给 undo 行的 caption 形如
            // "撤销 <操作名>" / "Undo <op>"，前缀是稳定的、本地化的。
            // 用"包含"会误命中任何带该词根的项（本地化词根重叠时尤其危险）。
            // 先剥掉前导的勾选标记/空白，再比前缀。
            std::wstring label = str::Trim(str::Lower(text));
            while (!label.empty() &&
                   (label.front() == L'✓' || label.front() == L'√' ||
                    label.front() == L'>' || label.front() == L' '))
                label.erase(0, 1);
            const bool isUndo = str::IStartsWith(label, L"undo") ||
                                str::IStartsWith(label, L"撤销") ||
                                str::IStartsWith(label, L"復原") ||
                                str::IStartsWith(label, L"取り消し");
            if (!isUndo) continue;
            CMINVOKECOMMANDINFOEX ici{};
            ici.cbSize = sizeof(ici);
            // 【不要传宿主窗口，也不要 SW_SHOWNORMAL】
            //
            // 这一条是被实测逼出来的：撤销动词偶尔会带出模态确认框（"确实要撤销
            // 删除吗 / 该操作无法撤销时改为永久删除"）。一旦传了 hwnd，
            // shell 会把它当 owner 去 **ModalLoop** —— 而本函数由 AI 通道在
            // **UI 线程**的定时器里调用（ai::PumpRequests），模态循环会连带
            // 卡死整个消息泵：界面不再重绘，管道后续请求也永远拿不到响应。
            // 实测现象：一次 file.undo 之后，同一连接上后续所有命令都返回空。
            //
            // 撤销本来也不需要人确认（它是"恢复"，不是破坏性操作），所以
            // 这里显式声明无 UI + 无 owner：让 shell 走它的静默路径。
            // 若某天真需要确认，应当由调用方先问人，而不是把模态塞进 UI 线程。
            ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI;
            ici.hwnd = nullptr;
            ici.lpVerb = MAKEINTRESOURCEA(id - 1);
            ici.lpVerbW = MAKEINTRESOURCEW(id - 1);
            ici.nShow = SW_SHOWNORMAL;
            done = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&ici)));
            break;
        }
    }
    DestroyMenu(menu);
    if (!done) LogDbg(L"[undo] 没有可撤销的操作（背景菜单里没有 undo 谓词）");
    return done;
}

bool RenameTo(IShellView* view, HWND owner, const std::wstring& fullPath,
              const std::wstring& newName, std::wstring* why) {
    // 外部路径 → shell 能解析的形式（正斜杠会让 SHCreateItemFromParsingName
    // 返回 E_INVALIDARG）。归一化之后 path 就是后面唯一使用的形式。
    const std::wstring path = pathutil::ToShellPath(fullPath);
    if (path.empty() || newName.empty()) {
        if (why) *why = L"参数为空（路径或新名）";
        return false;
    }
    // 名字里不能带路径分隔符 —— 那是"移动"，不是"重命名"。
    // 静默接受会让调用方以为挪了位置。
    if (newName.find(L'\\') != std::wstring::npos || newName.find(L'/') != std::wstring::npos) {
        if (why) *why = L"新名含路径分隔符（那是移动，不是重命名）";
        return false;
    }

    // 校验目标在当前视图里（避免用路径凭空操作视图外的目录）。
    //
    // 但**不靠"先选中再改名"**：IFileOperation::RenameItem 直接吃 IShellItem，
    // 不需要选中状态。早先的写法先 SelectByPath 再改名，有两个问题：
    //   ① 白改用户的选中状态（若随后失败，用户会莫名发现选中变了）；
    //   ② 选中会触发视图刷新，反而让后续操作更容易撞上竞态。
    // 这里改成"只读地确认目标存在"，不动选中。
    bool inView = false;
    {
        ComPtr<IFolderView2> fv2 = GetFv2(view);
        if (fv2) {
            ComPtr<IShellFolder> folder = GetFolder(fv2.get());
            if (folder) {
                int count = 0;
                if (SUCCEEDED(fv2->ItemCount(SVGIO_ALLVIEW, &count))) {
                    for (int i = 0; i < count && !inView; ++i) {
                        PITEMID_CHILD pidl = nullptr;
                        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
                        std::wstring p;
                        inView = ItemFullPath(folder.get(), pidl, &p) && SamePath(p, path);
                        CoTaskMemFree(pidl);
                    }
                }
            }
        }
    }
    if (!inView) {
        if (why) *why = L"目标不在当前视图：" + path;
        LogDbg(L"[shellops] RenameTo：目标不在当前视图 %s", path.c_str());
        return false;
    }

    std::wstring dir = ParentOf(path);
    std::wstring dest = dir.empty() ? newName : tf::Join(dir, newName);

    // 冲突检测：目标已存在则拒绝。IFileOperation 默认会问用户，
    // 但"确定式"调用不该弹框 —— 明确的失败比弹出对话框更可预期。
    if (tf::Exists(dest) && !SamePath(dest, path)) {
        if (why) *why = L"目标已存在：" + dest;
        LogDbg(L"[shellops] RenameTo：目标已存在 %s", dest.c_str());
        return false;
    }

    ComPtr<IShellItem> item;
    const HRESULT hrItem = SHCreateItemFromParsingName(
        path.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&item));
    if (FAILED(hrItem) || !item) {
        if (why) *why = L"无法定位源文件（HRESULT=0x" + HexHr(hrItem) + L"）：" + path;
        LogDbg(L"[shellops] RenameTo：SHCreateItemFromParsingName 失败 hr=0x%08X path=%s",
               static_cast<unsigned>(hrItem), path.c_str());
        return false;
    }

    ComPtr<IFileOperation> op;
    const HRESULT hrOp = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                          IID_IFileOperation, reinterpret_cast<void**>(&op));
    if (FAILED(hrOp) || !op) {
        if (why) *why = L"无法创建文件操作对象（HRESULT=0x" + HexHr(hrOp) + L"）";
        return false;
    }

    // 不要 UI：确定式操作遇到问题应当失败返回，而不是卡在一个没人看的对话框上。
    // FOF_ALLOWUNDO 让重命名可撤销（用户按 Ctrl+Z 能回去）。
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
    op->SetOwnerWindow(owner);
    const HRESULT hrRename = op->RenameItem(item.get(), newName.c_str(), nullptr);
    if (FAILED(hrRename)) {
        if (why) *why = L"重命名请求被拒（HRESULT=0x" + HexHr(hrRename) + L"）：" + newName;
        LogDbg(L"[shellops] RenameTo：RenameItem 失败 hr=0x%08X new=%s",
               static_cast<unsigned>(hrRename), newName.c_str());
        return false;
    }
    std::wstring opWhy;
    if (!RunOpEx(op.get(), &opWhy)) {
        if (why) *why = opWhy;
        LogDbg(L"[shellops] RenameTo：执行失败 %s", opWhy.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 按路径的文件操作
//
// 【为什么单独有一组】
// 撤销时，目标**已经不在当前视图里**了：移动过的文件到了别的目录，
// 复制出来的副本在别处。于是：
//   · "Selection" 语义（作用于用户碰巧选中的东西）用不了；
//   · RenameTo 的"目标必须在当前视图里"校验也过不了 ——
//     而那条校验的用意是"不要凭空操作视图外的目录"，撤销恰恰必须操作视图外。
//
// 所以这里另开一组**按完整路径**的文件操作，只被撤销栈调用，
// 不参与命令路径 —— 命令路径（file.* / fs.*）仍然只有一份实现。
//
// 共同点：都走 IFileOperation、都不弹 UI、冲突时**改名**而不是覆盖
//（FOF_RENAMEONCOLLISION），绝不静默覆盖用户文件。

namespace {

// 把一个完整路径的项移动/复制到目标目录（按路径，不依赖当前选中）。
bool TransferPath(HWND owner, const std::wstring& srcPathIn, const std::wstring& dstDirIn,
                   bool move) {
    // 路径归一化：撤销栈里的路径来自"当初那次操作"的记录，
    // 那里存的是归一化后的形式；这里再归一化一次是防御性的，
    // 保证"任何路径进 shell 都先过 ToShellPath"这条规则没有例外。
    const std::wstring srcPath = pathutil::ToShellPath(srcPathIn);
    const std::wstring dstDir = pathutil::ToShellPath(dstDirIn);
    if (srcPath.empty() || dstDir.empty()) return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(srcPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;
    ComPtr<IShellItem> dest;
    if (FAILED(SHCreateItemFromParsingName(dstDir.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&dest))) ||
        !dest)
        return false;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION);
    op->SetOwnerWindow(owner);
    const HRESULT hr = move ? op->MoveItem(item.get(), dest.get(), nullptr, nullptr)
                            : op->CopyItem(item.get(), dest.get(), nullptr, nullptr);
    if (FAILED(hr)) return false;
    return RunOp(op.get());
}

}  // namespace

bool RenamePath(HWND owner, const std::wstring& fullPathIn, const std::wstring& newName) {
    const std::wstring fullPath = pathutil::ToShellPath(fullPathIn);
    if (fullPath.empty() || newName.empty()) return false;
    // 名字里不能带路径分隔符 —— 那是"移动"，不是"重命名"。
    if (newName.find(L'\\') != std::wstring::npos || newName.find(L'/') != std::wstring::npos)
        return false;

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(fullPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;

    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION |
                          FOF_NOCONFIRMATION | FOF_NOERRORUI);
    op->SetOwnerWindow(owner);
    if (FAILED(op->RenameItem(item.get(), newName.c_str(), nullptr))) return false;
    return RunOp(op.get());
}

bool MovePathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir) {
    return TransferPath(owner, srcPath, dstDir, /*move=*/true);
}

bool CopyPathTo(HWND owner, const std::wstring& srcPath, const std::wstring& dstDir) {
    return TransferPath(owner, srcPath, dstDir, /*move=*/false);
}

bool DeletePath(HWND owner, const std::wstring& fullPathIn) {
    const std::wstring fullPath = pathutil::ToShellPath(fullPathIn);
    if (fullPath.empty()) return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(fullPath.c_str(), nullptr, IID_IShellItem,
                                           reinterpret_cast<void**>(&item))) ||
        !item)
        return false;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                reinterpret_cast<void**>(&op))) ||
        !op)
        return false;
    // FOF_WANTNUKEWARNING：超配额/网络盘时 shell 必须弹警告，不能静默永久删除。
    // 撤销"复制"就是删副本 —— 副本回回收站，随时能还原。
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_WANTNUKEWARNING);
    op->SetOwnerWindow(owner);
    if (FAILED(op->DeleteItem(item.get(), nullptr))) return false;
    return RunOp(op.get());
}

// ---------------------------------------------------------------- 只读查询

namespace {

// 从 IShellItem 读一组只读属性。失败项保持"未知"，不用 0 冒充。
void FillItemInfo(IShellItem* item, ItemInfo* info) {
    if (!item || !info) return;

    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &p)) && p) {
        info->name = p;
        CoTaskMemFree(p);
    }
    p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        info->path = p;
        CoTaskMemFree(p);
    }

    SFGAOF attrs = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_HIDDEN | SFGAO_READONLY, &attrs))) {
        info->isFolder = (attrs & SFGAO_FOLDER) != 0;
        info->hidden = (attrs & SFGAO_HIDDEN) != 0;
        info->readonly_ = (attrs & SFGAO_READONLY) != 0;
    }

    ComPtr<IShellItem2> i2;
    if (SUCCEEDED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&i2))) && i2) {
        UINT64 sz = 0;
        // 文件夹的 PKEY_Size 是"内容大小"，语义与文件不同 —— 只对文件报 size。
        if (!info->isFolder && SUCCEEDED(i2->GetUInt64(PKEY_Size, &sz))) {
            info->size = sz;
            info->sizeKnown = true;
        }
        FILETIME ft{};
        if (SUCCEEDED(i2->GetFileTime(PKEY_DateModified, &ft)) && ft.dwLowDateTime) {
            FileTimeToSystemTime(&ft, &info->modified);
            info->hasModified = true;
        }
        PWSTR t = nullptr;
        if (SUCCEEDED(i2->GetString(PKEY_ItemTypeText, &t)) && t) {
            info->typeName = t;
            CoTaskMemFree(t);
        }
    }
}

}  // namespace

bool ListView(IShellView* view, int limit, DirListing* out) {
    if (!view || !out) return false;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;

    out->path = ViewFolderPath(view);
    out->entries.clear();
    out->truncated = false;

    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return true;  // 空目录是合法结果

    for (int i = 0; i < count; ++i) {
        if (limit > 0 && static_cast<int>(out->entries.size()) >= limit) {
            out->truncated = true;
            break;
        }
        ComPtr<IShellItem> item;
        if (FAILED(fv2->GetItem(i, IID_IShellItem, reinterpret_cast<void**>(&item))) || !item)
            continue;
        DirEntry e;
        // DirEntry 是 ItemInfo 的子集，复用同一套读取再把要的字段搬过来。
        ItemInfo info;
        FillItemInfo(item.get(), &info);
        e.name = info.name;
        e.path = info.path;
        e.isFolder = info.isFolder;
        e.size = info.size;
        e.sizeKnown = info.sizeKnown;
        e.hidden = info.hidden;
        out->entries.push_back(std::move(e));
    }
    return true;
}

bool StatItem(IShellView* view, const std::wstring& fullPath, ItemInfo* out) {
    if (!view || fullPath.empty() || !out) return false;
    ComPtr<IFolderView2> fv2 = GetFv2(view);
    if (!fv2) return false;
    ComPtr<IShellFolder> folder = GetFolder(fv2.get());
    if (!folder) return false;

    int count = 0;
    if (FAILED(fv2->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return false;

    for (int i = 0; i < count; ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        std::wstring p;
        const bool got = ItemFullPath(folder.get(), pidl, &p);
        if (got && SamePath(p, fullPath)) {
            *out = ItemInfo{};
            // 用 pidl -> IShellItem，避免再按路径 Create（会有规范化差异）
            ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder.get(), pidl, IID_IShellItem,
                                                 reinterpret_cast<void**>(&item))) &&
                item)
                FillItemInfo(item.get(), out);
            CoTaskMemFree(pidl);
            return !out->name.empty();
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

}  // namespace shellops
