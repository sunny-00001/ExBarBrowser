#include "shortcut_clipboard.h"

#include "common.h"
#include "log.h"

#include <algorithm>
#include <cstring>

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace shortcut_clipboard {
namespace {

CLIPFORMAT DescriptorFormat() {
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
}
CLIPFORMAT ContentsFormat() {
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
}

struct Entry {
    std::wstring name;
    ComPtr<IStream> stream;
    ULARGE_INTEGER size{};
};

std::wstring LeafName(const std::wstring& path) {
    const wchar_t* leaf = PathFindFileNameW(path.c_str());
    return leaf && *leaf ? leaf : path;
}

// 同名不同目录的选中项会撞文件名；剪贴板里的 FILEDESCRIPTOR 必须唯一，
// 否则粘贴时后一个会覆盖前一个。
std::wstring UniqueName(const std::wstring& path, std::vector<std::wstring>& used) {
    const std::wstring base = LeafName(path);
    std::wstring name = base + L".lnk";
    for (int n = 2; std::find(used.begin(), used.end(), name) != used.end(); ++n)
        name = base + L" (" + std::to_wstring(n) + L").lnk";
    used.push_back(name);
    return name;
}

bool BuildEntry(const std::wstring& path, const std::wstring& name, Entry* out) {
    // 虚拟命名空间项（::{GUID}）没有可写的磁盘目标，SetPath 会失败；
    // 这类项直接跳过，不能生成一个"看起来成功但点了打不开"的快捷方式。
    if (path.empty() || path.rfind(L"::{", 0) == 0) return false;
    if (!PathFileExistsW(path.c_str())) {
        LogDbg(L"[path.copy.link] 跳过不存在的目标：%s", path.c_str());
        return false;
    }

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))))
        return false;
    if (FAILED(link->SetPath(path.c_str()))) return false;

    std::wstring work = path;
    PathRemoveFileSpecW(work.data());
    if (work.empty()) work = path;
    link->SetWorkingDirectory(work.c_str());
    link->SetDescription(path.c_str());

    ComPtr<IPersistStream> persist;
    if (FAILED(link->QueryInterface(IID_IPersistStream, reinterpret_cast<void**>(&persist))))
        return false;

    // 直接创建到目标成员：ComPtr 的移动赋值会触发它自己的 operator&，
    // 在这里实例化会编译失败，项目里此前也没有别处用到移动赋值。
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out->stream))) return false;
    if (FAILED(persist->Save(out->stream.get(), TRUE))) return false;
    LARGE_INTEGER zero{};
    if (FAILED(out->stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;

    STATSTG st{};
    if (FAILED(out->stream->Stat(&st, STATFLAG_NONAME))) return false;

    out->name = name;
    out->size = st.cbSize;
    return true;
}

// 延迟渲染剪贴板中要的两个格式：文件组描述符 + 按 lindex 索引的流。
class FormatEnum : public IEnumFORMATETC {
public:
    explicit FormatEnum(ULONG index = 0) : ref_(1), index_(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC* rgelt,
                                   ULONG* pceltFetched) override {
        if (!rgelt) return E_POINTER;
        ULONG n = 0;
        while (n < celt && index_ < 2) {
            rgelt[n++] = At(index_++);
        }
        if (pceltFetched) *pceltFetched = n;
        return n == celt ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override {
        index_ = (std::min)(index_ + celt, 2UL);
        return index_ == 2 ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** pp) override {
        if (!pp) return E_POINTER;
        *pp = new FormatEnum(index_);
        return S_OK;
    }

private:
    static FORMATETC At(ULONG i) {
        FORMATETC f{};
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        if (i == 0) {
            f.cfFormat = DescriptorFormat();
            f.tymed = TYMED_HGLOBAL;
        } else {
            f.cfFormat = ContentsFormat();
            f.tymed = TYMED_ISTREAM;
        }
        return f;
    }

    ULONG ref_ = 1;
    ULONG index_ = 0;
};

class ShortcutDataObject : public IDataObject {
public:
    explicit ShortcutDataObject(std::vector<Entry> entries)
        : ref_(1), entries_(std::move(entries)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pfe, STGMEDIUM* psm) override {
        if (!pfe || !psm) return E_POINTER;
        std::memset(psm, 0, sizeof(*psm));

        if ((pfe->tymed & TYMED_HGLOBAL) && pfe->cfFormat == DescriptorFormat() &&
            pfe->lindex == -1 && !entries_.empty()) {
            const UINT count = static_cast<UINT>(entries_.size());
            const size_t bytes =
                sizeof(FILEGROUPDESCRIPTORW) + (count - 1) * sizeof(FILEDESCRIPTORW);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!h) return E_OUTOFMEMORY;
            auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
            if (!group) {
                GlobalFree(h);
                return E_OUTOFMEMORY;
            }
            group->cItems = count;
            FILEDESCRIPTORW* fds = reinterpret_cast<FILEDESCRIPTORW*>(group + 1);
            std::memset(fds, 0, count * sizeof(FILEDESCRIPTORW));
            for (UINT i = 0; i < count; ++i) {
                FILEDESCRIPTORW& fd = fds[i];
                fd.dwFlags = FD_CLSID | FD_FILESIZE | FD_ATTRIBUTES;
                fd.clsid = CLSID_NULL;
                fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                fd.nFileSizeHigh = entries_[i].size.HighPart;
                fd.nFileSizeLow = entries_[i].size.LowPart;
                std::wcsncpy(fd.cFileName, entries_[i].name.c_str(),
                             _countof(fd.cFileName) - 1);
            }
            GlobalUnlock(h);
            psm->tymed = TYMED_HGLOBAL;
            psm->hGlobal = h;
            return S_OK;
        }

        if ((pfe->tymed & TYMED_ISTREAM) && pfe->cfFormat == ContentsFormat() &&
            pfe->lindex >= 0 && static_cast<size_t>(pfe->lindex) < entries_.size()) {
            // pUnkForRelease 留空 => ReleaseStgMedium 会 Release 这个流；
            // 这里先 AddRef，条目本身持有的那份要活到剪贴板被取代为止。
            IStream* stream = entries_[static_cast<size_t>(pfe->lindex)].stream.get();
            stream->AddRef();
            psm->tymed = TYMED_ISTREAM;
            psm->pstm = stream;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pfe) override {
        if (!pfe) return E_POINTER;
        if (pfe->cfFormat == DescriptorFormat() && (pfe->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (pfe->cfFormat == ContentsFormat() && (pfe->tymed & TYMED_ISTREAM) &&
            pfe->lindex >= 0 && static_cast<size_t>(pfe->lindex) < entries_.size())
            return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* pfeIn,
                                                    FORMATETC* pfeOut) override {
        if (!pfeIn || !pfeOut) return E_POINTER;
        *pfeOut = *pfeIn;
        pfeOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
                                            IEnumFORMATETC** pp) override {
        if (!pp) return E_POINTER;
        if (direction != DATADIR_GET) {
            *pp = nullptr;
            return E_NOTIMPL;
        }
        *pp = new FormatEnum();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                      DWORD*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return E_NOTIMPL;
    }

private:
    ULONG ref_ = 1;
    std::vector<Entry> entries_;
};

}  // namespace

bool CopyAsShortcut(const std::vector<std::wstring>& paths,
                    std::vector<std::wstring>* outNames) {
    std::vector<Entry> entries;
    entries.reserve(paths.size());
    std::vector<std::wstring> used;
    for (const auto& p : paths) {
        const std::wstring name = UniqueName(p, used);
        Entry entry;
        if (!BuildEntry(p, name, &entry)) {
            used.pop_back();  // 没成功就别占着这个名字
            continue;
        }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) return false;

    std::vector<std::wstring> names;
    names.reserve(entries.size());
    for (const auto& e : entries) names.push_back(e.name);

    auto* data = new ShortcutDataObject(std::move(entries));
    const HRESULT hr = OleSetClipboard(data);
    data->Release();  // OleSetClipboard 自己 AddRef 了
    if (FAILED(hr)) {
        LogDbg(L"[path.copy.link] OleSetClipboard 失败：0x%08X", hr);
        return false;
    }
    // 立刻 Flush：OleSetClipboard 只放"延迟渲染"的数据，进程退出后就没了。
    // Flush 后系统持有数据对象，延迟渲染的 IStream 仍由对象自己保管。
    if (FAILED(OleFlushClipboard())) return false;
    if (outNames) *outNames = std::move(names);
    LogDbg(L"[path.copy.link] 已放入 %zu 个快捷方式", names.size());
    return true;
}

}  // namespace shortcut_clipboard
