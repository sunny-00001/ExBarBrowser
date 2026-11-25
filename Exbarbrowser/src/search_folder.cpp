#include "search_folder.h"

#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <structuredquerycondition.h>

#include "log.h"
#include "path_util.h"

namespace {

// ---------------------------------------------------------------- GUID（本地）
// CLSID_ConditionFactory / IID_IConditionFactory2：MinGW 没有这两个接口的头
// （searchapi.h 与 structuredquerycondition.h 都不含），本地定义。
// 取值来自系统 SDK（与 exbar.cpp:25995 一致）。
const CLSID kCLSID_ConditionFactory = {
    0xe03e85b0, 0x7be3, 0x4000, {0xba, 0x98, 0x6c, 0x13, 0xde, 0x9f, 0xa4, 0x86}};
const IID kIID_IConditionFactory2 = {
    0x71d222e1, 0x432f, 0x429e, {0x8c, 0x13, 0xb6, 0xda, 0xfd, 0xe5, 0x07, 0x7a}};

// 条件工厂接口（vtable 顺序见下面的说明）。
struct ConditionFactory : public IUnknown {
    STDMETHOD(MakeNot)(ICondition* pcSub, BOOL fSimplify, ICondition** ppc) = 0;
    STDMETHOD(MakeAndOr)(CONDITION_TYPE ct, IEnumUnknown* peuSubs, BOOL fSimplify,
                         ICondition** ppc) = 0;
    STDMETHOD(MakeLeaf)(LPCWSTR pszPropertyName, CONDITION_OPERATION cop,
                        LPCWSTR pszValueType, const PROPVARIANT* ppropvar,
                        void* pPropertyNameTerm, void* pOperationTerm, void* pValueTerm,
                        BOOL fExpand, ICondition** ppc) = 0;
    STDMETHOD(ResolveDateTime)(ICondition* pc, SYSTEMTIME* pstReferenceTime,
                               BOOL fAlwaysOneInterval, DWORD sqro, ICondition** ppc) = 0;
    STDMETHOD(CreateTrueFalse)(BOOL fVal, DWORD cco, REFIID riid, void** ppv) = 0;
    STDMETHOD(CreateNegation)(ICondition* pcSub, DWORD cco, REFIID riid, void** ppv) = 0;
    STDMETHOD(CreateCompoundFromObjectArray)(CONDITION_TYPE ct, void* poaSubs, DWORD cco,
                                             REFIID riid, void** ppv) = 0;
    STDMETHOD(CreateCompoundFromArray)(CONDITION_TYPE ct, ICondition** ppcondSubs,
                                       ULONG cSubs, DWORD cco, REFIID riid, void** ppv) = 0;
    STDMETHOD(CreateStringLeaf)(REFPROPERTYKEY propkey, CONDITION_OPERATION cop,
                                LPCWSTR pszValue, LPCWSTR pszLocaleName, DWORD cco,
                                REFIID riid, void** ppv) = 0;
};

// 【vtable 顺序是实测出来的，不是照 SDK 抄的】
//
// 本机的条件工厂对象**不支持** IID_IConditionFactory（CoCreateInstance 返回
// E_NOINTERFACE），只支持 IID_IConditionFactory2；而它的实际槽位顺序是
// "IConditionFactory2 的四个方法在前、IConditionFactory 的五个方法在后"。
// 按 SDK 那种"派生接口接在基类之后"的常规排法声明，CreateStringLeaf 会调到
// MakeNot 的槽位上 —— 不报编译错，只是 HRESULT 莫名其妙、条件建不出来。
//
// 证据在 tools/probe_search.cpp：两种排法都试一遍，用返回值判定哪种对
// （实测：[A] 常规排法 QI 直接 E_NOINTERFACE；[B] 本文的排法 CreateStringLeaf
//  返回 S_OK，且 GetConditionType 给出 CT_LEAF_CONDITION）。
// **改这个顺序前先跑一遍探针**，别凭"SDK 应该是这样"改。

// 建一个"属性 = 字符串"的叶子条件。失败返回 nullptr。
ICondition* MakeLeaf(ConditionFactory* fac, REFPROPERTYKEY key, CONDITION_OPERATION op,
                     const std::wstring& value) {
    ICondition* cond = nullptr;
    HRESULT hr = fac->CreateStringLeaf(key, op, value.c_str(), LOCALE_NAME_INVARIANT, 0,
                                        IID_ICondition, reinterpret_cast<void**>(&cond));
    if (FAILED(hr) || !cond) {
        LogDbg(L"[sfolder] CreateStringLeaf hr=0x%08lX", static_cast<unsigned long>(hr));
        return nullptr;
    }
    return cond;
}

// 条件树：文件名包含 query；onlyThisLayer 时再要求"父目录 == scopeDir"。
// 返回的 ICondition 由调用者 Release。
ICondition* BuildCondition(const std::wstring& query, const std::wstring& scopeDir,
                            bool onlyThisLayer) {
    ConditionFactory* fac = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  kIID_IConditionFactory2, reinterpret_cast<void**>(&fac));
    if (FAILED(hr) || !fac) {
        LogDbg(L"[sfolder] 条件工厂不可用 hr=0x%08lX", static_cast<unsigned long>(hr));
        return nullptr;
    }

    ICondition* nameCond = MakeLeaf(fac, PKEY_FileName, COP_VALUE_CONTAINS, query);
    if (!nameCond) {
        fac->Release();
        return nullptr;
    }

    ICondition* result = nameCond;
    if (onlyThisLayer && !scopeDir.empty()) {
        // "仅本层"：父目录恰好等于 scopeDir 的项。
        // 作用域（SetScope）天生递归，所以这一层只能靠条件圈。
        ICondition* parentCond =
            MakeLeaf(fac, PKEY_ItemFolderPathDisplay, COP_EQUAL, scopeDir);
        if (parentCond) {
            ICondition* subs[2] = {nameCond, parentCond};
            ICondition* both = nullptr;
            hr = fac->CreateCompoundFromArray(CT_AND_CONDITION, subs, 2, 0, IID_ICondition,
                                               reinterpret_cast<void**>(&both));
            if (SUCCEEDED(hr) && both) {
                nameCond->Release();
                result = both;
            } else {
                LogDbg(L"[sfolder] 复合条件失败 hr=0x%08lX", static_cast<unsigned long>(hr));
                parentCond->Release();
            }
        }
    }
    fac->Release();
    return result;
}

}  // namespace

bool searchfolder::Available() {
    ISearchFolderItemFactory* sff = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ISearchFolderItemFactory, reinterpret_cast<void**>(&sff));
    if (FAILED(hr) || !sff) return false;
    sff->Release();
    return true;
}

bool searchfolder::Create(const std::wstring& query, const std::wstring& scopeDir,
                          bool onlyThisLayer, const std::wstring& displayName,
                          PIDLIST_ABSOLUTE* outPidl) {
    if (!outPidl || query.empty()) return false;
    *outPidl = nullptr;

    // 作用域必须是**一个**普通文件系统目录。整机范围做不到（见头文件），
    // 所以这里不接空目录：空 = 调用方该报"整机范围不可用"，不是悄悄降级。
    if (scopeDir.empty()) {
        LogDbg(L"[sfolder] 没有作用域：整机范围在本宿主不可用");
        return false;
    }

    ISearchFolderItemFactory* sff = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ISearchFolderItemFactory, reinterpret_cast<void**>(&sff));
    if (FAILED(hr) || !sff) {
        LogDbg(L"[sfolder] 搜索文件夹工厂不可用 hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    ICondition* cond = BuildCondition(query, scopeDir, onlyThisLayer);
    if (!cond) {
        sff->Release();
        return false;
    }

    bool ok = false;
    hr = sff->SetCondition(cond);
    if (FAILED(hr)) {
        LogDbg(L"[sfolder] SetCondition hr=0x%08lX", static_cast<unsigned long>(hr));
        sff->Release();
        cond->Release();
        return false;
    }

    // 作用域：限定到某个目录。
    //
    // 【尾斜杠是必须的，不是讲究】不带尾斜杠时 SHParseDisplayName 会把
    // "C:\Users\X" 规范化成 CSIDL_PROFILE 那个 **shell 命名空间项**，搜索服务
    // 不认这种作用域 —— 表现是 BrowseToObject 返回 S_OK、视图留在原地，
    // 随后 OnNavigationFailed。带尾斜杠走的是普通文件夹路径，立刻能绑。
    // （实测：C:\Users\X 不行、C:\Users\X\ 行；C:\Windows\ 行。）
    {
        std::wstring scope = pathutil::ToShellPath(scopeDir);
        if (scope.empty() || scope.back() != L'\\') scope += L'\\';
        PIDLIST_ABSOLUTE abs = nullptr;
        hr = SHParseDisplayName(scope.c_str(), nullptr, &abs, 0, nullptr);
        if (FAILED(hr) || !abs) {
            LogDbg(L"[sfolder] 作用域解析失败 '%s' hr=0x%08lX", scope.c_str(),
                   static_cast<unsigned long>(hr));
        } else {
            PCIDLIST_ABSOLUTE scopeList = abs;
            IShellItemArray* sia = nullptr;
            hr = SHCreateShellItemArrayFromIDLists(1, &scopeList, &sia);
            CoTaskMemFree(abs);
            if (SUCCEEDED(hr) && sia) {
                hr = sff->SetScope(sia);
                if (FAILED(hr))
                    LogDbg(L"[sfolder] SetScope('%s') hr=0x%08lX", scope.c_str(),
                           static_cast<unsigned long>(hr));
                sia->Release();
            } else {
                LogDbg(L"[sfolder] 作用域数组 hr=0x%08lX", static_cast<unsigned long>(hr));
            }
        }
    }

    hr = sff->SetDisplayName(displayName.c_str());
    if (FAILED(hr)) {
        LogDbg(L"[sfolder] SetDisplayName hr=0x%08lX", static_cast<unsigned long>(hr));
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = sff->GetIDList(&pidl);
    if (SUCCEEDED(hr) && pidl) {
        *outPidl = pidl;
        ok = true;
        // 身份指纹：同一批条件造出来的 PIDL 是同一份吗？
        // （shell 靠**解析名**认"是不是同一个搜索文件夹" —— 两次搜索显示名相同
        //  就会被当成同一个文件夹直接复用旧结果，换范围也不刷新。所以标题必须
        //  随范围变，见 App::RunSearch。）
        unsigned hash = 2166136261u;
        for (BYTE* b = reinterpret_cast<BYTE*>(pidl); b && *b; ++b)
            hash = (hash ^ *b) * 16777619u;
        PWSTR nm = nullptr;
        std::wstring name;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &nm)) && nm) {
            name = nm;
            CoTaskMemFree(nm);
        }
        LogDbg(L"[sfolder] pidl name=%s hash=%08X", name.c_str(), hash);
    } else {
        LogDbg(L"[sfolder] GetIDList hr=0x%08lX", static_cast<unsigned long>(hr));
    }

    cond->Release();
    sff->Release();
    return ok;
}
