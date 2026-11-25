// 探针：验证 Windows Search 搜索文件夹这条路在本机可用。
//
// 要回答三件"猜不得"的事：
//   1. 本机有没有可用的 Windows Search（CoCreateInstance CLSID_SearchFolderItemFactory）。
//   2. IConditionFactory 的 vtable 顺序。MinGW 没有这个接口的头（searchapi.h 与
//      structuredquerycondition.h 都没有），只能本地声明 —— **写错不报编译错**，
//      但调用会打到别的槽位上，症状是"条件建不出来"或 HRESULT 乱七八糟。
//      这里把两种可能的排法都试一遍，用返回值判定哪种对。
//   3. 造出来的 PIDL 解析名是不是 "search-ms:..."（搜索视图就位判定要用它）。
//
// 只读：不写文件、不改用户数据、不碰注册表。
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propkey.h>
#include <structuredquerycondition.h>

#include <stdio.h>
#include <string>

// ---------------------------------------------------------------- GUID（本地）
static const CLSID kCLSID_ConditionFactory = {
    0xe03e85b0, 0x7be3, 0x4000, {0xba, 0x98, 0x6c, 0x13, 0xde, 0x9f, 0xa4, 0x86}};
static const IID kIID_IConditionFactory = {
    0x4fdef69c, 0xdbc9, 0x454e, {0x99, 0x10, 0xb3, 0x4f, 0x3c, 0x64, 0xb5, 0x10}};
static const IID kIID_IConditionFactory2 = {
    0x71d222e1, 0x432f, 0x429e, {0x8c, 0x13, 0xb6, 0xda, 0xfd, 0xe5, 0x07, 0x7a}};

// ------------------------------------------------- 两种 vtable 排法的本地声明
// 排法 A（SDK 常规：派生接口的槽位接在基类之后）
struct FactoryA : public IUnknown {
    STDMETHOD(CreateTrueFalse)(BOOL, DWORD, REFIID, void**) = 0;
    STDMETHOD(CreateNegation)(ICondition*, DWORD, REFIID, void**) = 0;
    STDMETHOD(CreateCompoundFromObjectArray)(CONDITION_TYPE, IUnknown*, DWORD, REFIID,
                                             void**) = 0;
    STDMETHOD(CreateCompoundFromArray)(CONDITION_TYPE, ICondition**, ULONG, DWORD, REFIID,
                                       void**) = 0;
    STDMETHOD(CreateStringLeaf)(REFPROPERTYKEY, CONDITION_OPERATION, LPCWSTR, LPCWSTR, DWORD,
                                REFIID, void**) = 0;
};

// 排法 B（IConditionFactory2 的四个新方法在前，基类的五个在后）
struct FactoryB : public IUnknown {
    STDMETHOD(MakeNot)(ICondition*, BOOL, ICondition**) = 0;
    STDMETHOD(MakeAndOr)(CONDITION_TYPE, IEnumUnknown*, BOOL, ICondition**) = 0;
    STDMETHOD(MakeLeaf)(LPCWSTR, CONDITION_OPERATION, LPCWSTR, const PROPVARIANT*, void*, void*,
                        void*, BOOL, ICondition**) = 0;
    STDMETHOD(ResolveDateTime)(ICondition*, SYSTEMTIME*, BOOL, DWORD, ICondition**) = 0;
    STDMETHOD(CreateTrueFalse)(BOOL, DWORD, REFIID, void**) = 0;
    STDMETHOD(CreateNegation)(ICondition*, DWORD, REFIID, void**) = 0;
    STDMETHOD(CreateCompoundFromObjectArray)(CONDITION_TYPE, IUnknown*, DWORD, REFIID,
                                             void**) = 0;
    STDMETHOD(CreateCompoundFromArray)(CONDITION_TYPE, ICondition**, ULONG, DWORD, REFIID,
                                       void**) = 0;
    STDMETHOD(CreateStringLeaf)(REFPROPERTYKEY, CONDITION_OPERATION, LPCWSTR, LPCWSTR, DWORD,
                                REFIID, void**) = 0;
};

static void Say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(buf, 1, static_cast<size_t>(n), stdout);
    fflush(stdout);
}

static void SayHr(const char* what, HRESULT hr) {
    Say("%s hr=0x%08lX %s\n", what, static_cast<unsigned long>(hr),
        SUCCEEDED(hr) ? "OK" : "FAIL");
}

// 用某种排法建一个叶子条件；成功返回条件指针（调用方 Release）。
static ICondition* MakeLeaf(void* fac, bool layoutA, const wchar_t* value) {
    ICondition* cond = nullptr;
    if (layoutA) {
        auto* f = static_cast<FactoryA*>(fac);
        HRESULT hr = f->CreateStringLeaf(PKEY_FileName, COP_VALUE_CONTAINS, value,
                                          LOCALE_NAME_INVARIANT, 0, IID_ICondition,
                                          reinterpret_cast<void**>(&cond));
        Say("  [A] CreateStringLeaf hr=0x%08lX\n", static_cast<unsigned long>(hr));
    } else {
        auto* f = static_cast<FactoryB*>(fac);
        HRESULT hr = f->CreateStringLeaf(PKEY_FileName, COP_VALUE_CONTAINS, value,
                                          LOCALE_NAME_INVARIANT, 0, IID_ICondition,
                                          reinterpret_cast<void**>(&cond));
        Say("  [B] CreateStringLeaf hr=0x%08lX\n", static_cast<unsigned long>(hr));
    }
    return cond;
}

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SayHr("CoInitializeEx", hr);

    // ---- 1. 搜索文件夹工厂 ----------------------------------------------
    ISearchFolderItemFactory* sff = nullptr;
    hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ISearchFolderItemFactory, reinterpret_cast<void**>(&sff));
    SayHr("CoCreateInstance(ISearchFolderItemFactory)", hr);
    if (FAILED(hr) || !sff) {
        Say("结论：本机没有可用的 Windows Search 搜索文件夹工厂。\n");
        CoUninitialize();
        return 2;
    }

    // ---- 2. 条件工厂：两种排法都试 --------------------------------------
    Say("\n== 条件工厂 ==\n");
    IUnknown* facBase = nullptr;
    hr = CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                          kIID_IConditionFactory, reinterpret_cast<void**>(&facBase));
    SayHr("CoCreateInstance(IConditionFactory)", hr);

    IUnknown* fac2 = nullptr;
    HRESULT hr2 = CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   kIID_IConditionFactory2, reinterpret_cast<void**>(&fac2));
    SayHr("CoCreateInstance(IConditionFactory2)", hr2);

    bool leafOk = false;
    if (SUCCEEDED(hr) && facBase) {
        ICondition* c = MakeLeaf(facBase, true, L"probe");
        if (c) {
            CONDITION_TYPE ct = CT_LEAF_CONDITION;
            HRESULT h = c->GetConditionType(&ct);
            Say("  [A] GetConditionType hr=0x%08lX type=%d (3=LEAF)\n",
                static_cast<unsigned long>(h), static_cast<int>(ct));
            leafOk = SUCCEEDED(h) && ct == CT_LEAF_CONDITION;
            c->Release();
        }
        facBase->Release();
    }
    if (!leafOk && SUCCEEDED(hr2) && fac2) {
        ICondition* c = MakeLeaf(fac2, false, L"probe");
        if (c) {
            CONDITION_TYPE ct = CT_LEAF_CONDITION;
            HRESULT h = c->GetConditionType(&ct);
            Say("  [B] GetConditionType hr=0x%08lX type=%d (3=LEAF)\n",
                static_cast<unsigned long>(h), static_cast<int>(ct));
            leafOk = SUCCEEDED(h) && ct == CT_LEAF_CONDITION;
            c->Release();
        }
        fac2->Release();
    }
    Say("叶子条件可用：%s\n", leafOk ? "是" : "否");

    // ---- 3. 造一个搜索文件夹，看 PIDL 的解析名 ---------------------------
    Say("\n== 搜索文件夹 ==\n");
    IUnknown* fac = nullptr;
    IUnknown* facForLeaf = nullptr;
    // 复用一个能建条件的工厂
    if (SUCCEEDED(hr) && facBase == nullptr) {
        // 上面已 Release，这里重新拿一份
        CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                         kIID_IConditionFactory, reinterpret_cast<void**>(&fac));
        facForLeaf = fac;
    }
    ICondition* cond = nullptr;
    if (facForLeaf) {
        cond = MakeLeaf(facForLeaf, true, L"probe");
    } else if (SUCCEEDED(hr2)) {
        CoCreateInstance(kCLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                         kIID_IConditionFactory2, reinterpret_cast<void**>(&fac));
        facForLeaf = fac;
        if (fac) cond = MakeLeaf(fac, false, L"probe");
    }

    if (!cond) {
        Say("建不出条件，无法继续。\n");
        sff->Release();
        CoUninitialize();
        return 3;
    }

    hr = sff->SetCondition(cond);
    SayHr("SetCondition", hr);

    // 作用域：限定到本目录
    PIDLIST_ABSOLUTE dir = nullptr;
    hr = SHParseDisplayName(L"E:\\ExBarBrowser", nullptr, &dir, 0, nullptr);
    SayHr("SHParseDisplayName(scope)", hr);
    if (SUCCEEDED(hr) && dir) {
        IShellItemArray* sia = nullptr;
        PCIDLIST_ABSOLUTE scopeList = dir;
        hr = SHCreateShellItemArrayFromIDLists(1, &scopeList, &sia);
        SayHr("SHCreateShellItemArrayFromIDLists", hr);
        if (SUCCEEDED(hr) && sia) {
            hr = sff->SetScope(sia);
            SayHr("SetScope", hr);
            sia->Release();
        }
        CoTaskMemFree(dir);
    }

    hr = sff->SetDisplayName(L"搜索结果");
    SayHr("SetDisplayName", hr);

    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = sff->GetIDList(&pidl);
    SayHr("GetIDList", hr);
    if (SUCCEEDED(hr) && pidl) {
        PWSTR nm = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &nm)) && nm) {
            char narrow[1024] = {0};
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            Say("PIDL 解析名 = %s\n", narrow);
            CoTaskMemFree(nm);
        }
        // 从 PIDL 造 IShellItem（我们要用它导航）
        IShellItem* item = nullptr;
        hr = SHCreateItemFromIDList(pidl, IID_IShellItem, reinterpret_cast<void**>(&item));
        SayHr("SHCreateItemFromIDList", hr);
        if (SUCCEEDED(hr) && item) {
            PWSTR inm = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &inm)) && inm) {
                char narrow[1024] = {0};
                WideCharToMultiByte(CP_UTF8, 0, inm, -1, narrow, sizeof(narrow) - 1, nullptr,
                                    nullptr);
                Say("IShellItem 解析名 = %s\n", narrow);
                CoTaskMemFree(inm);
            }
            item->Release();
        }
        CoTaskMemFree(pidl);
    }

    cond->Release();
    if (facForLeaf) facForLeaf->Release();
    sff->Release();
    CoUninitialize();
    Say("\n探针结束。\n");
    return 0;
}
