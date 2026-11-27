// 自绘栏 UIA provider 实现。见 tab_uia.h 的设计说明（标签栏/书签栏共用）。
//
// 线程模型：UIA 客户端可能从任意线程查询。条目数据只属于 UI 线程，
// 因此所有读取与选中都 SendMessage 回 UI 线程（同步、同一进程，指针在返回前有效），
// 不在 provider 里直接碰宿主类的成员 —— provider 只持有 Traits（根名/前缀）。
#include "tab_uia.h"

#include "tab_strip.h"

#include <uiautomationcore.h>
#include <uiautomationcoreapi.h>

// TabTraitsFor：标签栏的 Traits 具体值（书签栏的见 bookmark_bar_uia.cpp）。
uia::Traits uia::TabTraitsFor(TabStrip* s) {
    Traits t;
    t.rootName = L"标签栏";
    t.rootControlType = 50018;              // UIA_TabControlTypeId
    t.automationPrefix = L"tab.";
    t.owner = s;
    return t;
}

// 不包含 uiautomationclient.h：它含 C 风格的 `enum X` 前向声明，
// clang 在 C++ 模式下直接报错（ISO C++ forbids forward references to 'enum'）。
// 这里只用到少量属性/控件类型 id，按 SDK 值本地登记，语义与官方头一致。
#ifndef UIA_NamePropertyId
#define UIA_NamePropertyId 30005
#define UIA_ControlTypePropertyId 30003
#define UIA_AutomationIdPropertyId 30011
#define UIA_HelpTextPropertyId 30013
#define UIA_IsControlElementPropertyId 30016
#define UIA_IsContentElementPropertyId 30017
#define UIA_IsEnabledPropertyId 30010
#define UIA_IsKeyboardFocusablePropertyId 30009
#define UIA_HasKeyboardFocusPropertyId 30008
#define UIA_TabControlTypeId 50018
#define UIA_TabItemControlTypeId 50019
#define UIA_SelectionItemPatternId 10010
#define UIA_InvokePatternId 10000
#endif

#include <oleauto.h>

#include <cstdlib>
#include <map>
#include <mutex>

namespace {

// 本地 IID：不依赖 uuid.lib 是否收录 UIA 的 GUID。
const GUID kIID_Simple = {0xd6dd68d1, 0x86fd, 0x4332, {0x86, 0x66, 0x9a, 0xbe, 0xde, 0xa2, 0xd2, 0x4c}};
const GUID kIID_Fragment = {0xf7063da8, 0x8359, 0x439c, {0x92, 0x97, 0xbb, 0xc5, 0x29, 0x9a, 0x7d, 0x87}};
const GUID kIID_FragmentRoot = {0x620ce2a5, 0xab8f, 0x40a9, {0x86, 0xcb, 0xde, 0x3c, 0x75, 0x59, 0x9b, 0x58}};
const GUID kIID_SelectionItem = {0x2acad808, 0xb2d4, 0x452d, {0xa4, 0x07, 0x91, 0xff, 0x1a, 0xd1, 0x67, 0xb2}};
const GUID kIID_Invoke = {0x54fcb24b, 0xe18e, 0x47a2, {0xb4, 0xd3, 0xec, 0xcb, 0xe7, 0x75, 0x99, 0xa2}};

class TabRootProvider;

std::mutex g_mtx;
std::map<HWND, TabRootProvider*> g_roots;

SAFEARRAY* MakeRuntimeId(HWND hwnd, int child) {
    SAFEARRAY* a = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!a) return nullptr;
    LONG* p = nullptr;
    if (FAILED(SafeArrayAccessData(a, reinterpret_cast<void**>(&p)))) {
        SafeArrayDestroy(a);
        return nullptr;
    }
    p[0] = static_cast<LONG>(reinterpret_cast<ULONG_PTR>(hwnd));
    p[1] = child;
    SafeArrayUnaccessData(a);
    return a;
}

void SetString(VARIANT* v, const wchar_t* s) {
    VariantInit(v);
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocString(s ? s : L"");
}

void SetInt(VARIANT* v, LONG n) {
    VariantInit(v);
    v->vt = VT_I4;
    v->lVal = n;
}

void SetBool(VARIANT* v, BOOL b) {
    VariantInit(v);
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

class TabItemProvider final : public IRawElementProviderSimple,
                              public IRawElementProviderFragment,
                              public ISelectionItemProvider,
                              public IInvokeProvider {
public:
    TabItemProvider(HWND hwnd, int index, const uia::Traits& traits)
        : hwnd_(hwnd), index_(index), traits_(traits) {}
    ~TabItemProvider() = default;

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_Simple))
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        else if (IsEqualIID(riid, kIID_Fragment))
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        else if (IsEqualIID(riid, kIID_SelectionItem))
            *ppv = static_cast<ISelectionItemProvider*>(this);
        else if (IsEqualIID(riid, kIID_Invoke))
            *ppv = static_cast<IInvokeProvider*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    // ---- IRawElementProviderSimple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        if (id == UIA_SelectionItemPatternId) {
            *pRetVal = static_cast<ISelectionItemProvider*>(this);
            AddRef();
        } else if (id == UIA_InvokePatternId) {
            *pRetVal = static_cast<IInvokeProvider*>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        uia::ItemInfo info;
        QueryInfo(&info);
        switch (id) {
        case UIA_NamePropertyId: SetString(pRetVal, info.name.c_str()); break;
        case UIA_ControlTypePropertyId: SetInt(pRetVal, UIA_TabItemControlTypeId); break;
        case UIA_AutomationIdPropertyId: {
            const std::wstring s = std::wstring(traits_.automationPrefix) +
                                   std::to_wstring(index_);
            SetString(pRetVal, s.c_str());
            break;
        }
        case UIA_HelpTextPropertyId: SetString(pRetVal, info.path.c_str()); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId:
        case UIA_IsEnabledPropertyId:
        case UIA_IsKeyboardFocusablePropertyId: SetBool(pRetVal, TRUE); break;
        // HasKeyboardFocus 是"键盘焦点在这个元素上"，不是"这是活动标签"。
        // 活动标签在标签栏失去焦点时仍 selected，但焦点已在别处 ——
        // 把 selected 当焦点报会让读屏软件把无焦点的项读成"当前焦点"。
        case UIA_HasKeyboardFocusPropertyId:
            SetBool(pRetVal, info.selected && GetFocus() == hwnd_);
            break;
        default: VariantInit(pRetVal); break;   // 不支持 → VT_EMPTY
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }

    // ---- IRawElementProviderFragment ----
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir,
                                       IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        uia::ItemInfo info;
        QueryInfo(&info);
        POINT pts[2] = {{info.rect.left, info.rect.top}, {info.rect.right, info.rect.bottom}};
        MapWindowPoints(hwnd_, nullptr, pts, 2);
        pRetVal->left = pts[0].x;
        pRetVal->top = pts[0].y;
        pRetVal->width = pts[1].x - pts[0].x;
        pRetVal->height = pts[1].y - pts[0].y;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = MakeRuntimeId(hwnd_, index_ + 1);
        return *pRetVal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        SendMessageW(hwnd_, uia::kMsgSelect, static_cast<WPARAM>(index_), 0);
        ::SetFocus(hwnd_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }

    // ---- ISelectionItemProvider ----
    HRESULT STDMETHODCALLTYPE Select() override {
        SendMessageW(hwnd_, uia::kMsgSelect, static_cast<WPARAM>(index_), 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return S_OK; }   // 单选容器
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        uia::ItemInfo info;
        QueryInfo(&info);
        *pRetVal = info.selected ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) override;

    // ---- IInvokeProvider ----
    // 【E23 修复】原先与 Select() 逐字相同（都发 kMsgSelect），于是"激活"退化成
    // "只把焦点移过去"。UIA 里 Invoke 是"执行默认动作"，与 Select 不同：
    // 标签栏 = 切到这个标签，书签栏 = 打开这个书签。由两栏各自的 kMsgInvoke
    // 分支决定具体做什么（provider 与栏类型无关，不在这里判）。
    HRESULT STDMETHODCALLTYPE Invoke() override {
        SendMessageW(hwnd_, uia::kMsgInvoke, static_cast<WPARAM>(index_), 0);
        return S_OK;
    }

private:
    TabRootProvider* Root();
    int Count() const {
        return static_cast<int>(SendMessageW(hwnd_, uia::kMsgCount, 0, 0));
    }
    void QueryInfo(uia::ItemInfo* info) const {
        if (info) SendMessageW(hwnd_, uia::kMsgItem, static_cast<WPARAM>(index_),
                               reinterpret_cast<LPARAM>(info));
    }

    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
    int index_ = 0;
    uia::Traits traits_;   // automationId 前缀来自这里（tab./bmk.）
};

class TabRootProvider final : public IRawElementProviderSimple,
                              public IRawElementProviderFragment,
                              public IRawElementProviderFragmentRoot {
public:
    TabRootProvider(HWND hwnd, const uia::Traits& traits) : hwnd_(hwnd), traits_(traits) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, kIID_FragmentRoot))
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (IsEqualIID(riid, kIID_Fragment))
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        else if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_Simple))
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) {
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                auto it = g_roots.find(hwnd_);
                if (it != g_roots.end() && it->second == this) g_roots.erase(it);
            }
            delete this;
        }
        return n;
    }

    // ---- Simple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        switch (id) {
        case UIA_NamePropertyId: SetString(pRetVal, traits_.rootName); break;
        case UIA_ControlTypePropertyId: SetInt(pRetVal, UIA_TabControlTypeId); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId:
        case UIA_IsEnabledPropertyId: SetBool(pRetVal, TRUE); break;
        default: VariantInit(pRetVal); break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }

    // ---- FragmentRoot ----
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                       IRawElementProviderFragment** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
        ScreenToClient(hwnd_, &pt);
        const int n = Count();
        for (int i = 0; i < n; ++i) {
            uia::ItemInfo info;
            QueryInfo(i, &info);
            if (PtInRect(&info.rect, pt)) {
                *pRetVal = new TabItemProvider(hwnd_, i, traits_);
                return S_OK;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        const int n = Count();
        for (int i = 0; i < n; ++i) {
            uia::ItemInfo info;
            QueryInfo(i, &info);
            if (info.selected) {
                *pRetVal = new TabItemProvider(hwnd_, i, traits_);
                break;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        ::SetFocus(hwnd_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir,
                                       IRawElementProviderFragment** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = nullptr;
        const int n = Count();
        if (n == 0) return S_OK;
        if (dir == NavigateDirection_FirstChild)
            *pRetVal = new TabItemProvider(hwnd_, 0, traits_);
        else if (dir == NavigateDirection_LastChild)
            *pRetVal = new TabItemProvider(hwnd_, n - 1, traits_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        POINT pts[2] = {{rc.left, rc.top}, {rc.right, rc.bottom}};
        MapWindowPoints(hwnd_, nullptr, pts, 2);
        pRetVal->left = pts[0].x;
        pRetVal->top = pts[0].y;
        pRetVal->width = pts[1].x - pts[0].x;
        pRetVal->height = pts[1].y - pts[0].y;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override {
        if (!pRetVal) return E_INVALIDARG;
        *pRetVal = MakeRuntimeId(hwnd_, 0);
        return *pRetVal ? S_OK : E_OUTOFMEMORY;
    }

private:
    int Count() const { return static_cast<int>(SendMessageW(hwnd_, uia::kMsgCount, 0, 0)); }
    void QueryInfo(int i, uia::ItemInfo* info) const {
        SendMessageW(hwnd_, uia::kMsgItem, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(info));
    }

    LONG refs_ = 1;
    HWND hwnd_ = nullptr;
    uia::Traits traits_;   // 根名（"标签栏"/"书签栏"）来自这里
};

// Root()：查根 provider 并**带着引用返回**（锁内 AddRef，调用方用完要 Release）。
// 原实现锁内只取裸指针、调用方锁外 AddRef —— 两步之间根窗口销毁、provider
// 引用归零被 delete，AddRef 写的是已释放内存（UAF）。AddRef 必须在锁内完成，
// 让"找到"与"留住"原子化。
TabRootProvider* TabItemProvider::Root() {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_roots.find(hwnd_);
    if (it == g_roots.end()) return nullptr;
    it->second->AddRef();
    return it->second;
}

// 以下三个方法的实现放在 TabRootProvider 完整定义之后：
// 它们需要把 Root() 转成具体接口，前半段只有前向声明，编译器看不到继承关系。
HRESULT TabItemProvider::Navigate(NavigateDirection dir, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_INVALIDARG;
    *pRetVal = nullptr;
    if (dir == NavigateDirection_Parent) {
        // Root() 已带引用返回，这里 AddRef 会凭空多出一层导致内存泄漏。
        TabRootProvider* root = Root();
        if (root) {
            *pRetVal = static_cast<IRawElementProviderFragment*>(root);
        }
        return S_OK;
    }
    const int n = Count();
    int target = -1;
    if (dir == NavigateDirection_NextSibling && index_ + 1 < n) target = index_ + 1;
    if (dir == NavigateDirection_PreviousSibling && index_ > 0) target = index_ - 1;
    if (target >= 0) *pRetVal = new TabItemProvider(hwnd_, target, traits_);
    return S_OK;
}

HRESULT TabItemProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) return E_INVALIDARG;
    // Root() 带引用返回，转入接口指针不再额外 AddRef。
    *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(Root());
    return S_OK;
}

HRESULT TabItemProvider::get_SelectionContainer(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) return E_INVALIDARG;
    *pRetVal = static_cast<IRawElementProviderSimple*>(Root());
    return S_OK;
}

}  // namespace

namespace uia {

LRESULT HandleGetObject(const Traits& traits, HWND hwnd, WPARAM w, LPARAM l) {
    if (!traits.owner || !hwnd) return 0;
    // 【诊断开关】EXBAR_NO_UIA_PROVIDER=1 → 不提供 provider（隔离 UIA 死锁用）。
    // 只影响 provider 是否挂上，不改其它行为；排查完可去掉。
    static const bool kDisabled =
        (std::getenv("EXBAR_NO_UIA_PROVIDER") != nullptr);
    if (kDisabled) return 0;
    TabRootProvider* root = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        auto it = g_roots.find(hwnd);
        if (it == g_roots.end()) {
            root = new TabRootProvider(hwnd, traits);
            g_roots[hwnd] = root;
        } else {
            root = it->second;
        }
        root->AddRef();   // 锁内留住：与 Release(hwnd) 的竞态从这里开始被引用计数挡住
    }
    const LRESULT lr = UiaReturnRawElementProvider(hwnd, w, l, root);
    root->Release();      // 归还借出引用；UIA 那份由它自己持有
    return lr;
}

void Release(HWND hwnd) {
    TabRootProvider* root = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        auto it = g_roots.find(hwnd);
        if (it != g_roots.end()) {
            root = it->second;
            g_roots.erase(it);
        }
    }
    if (root) root->Release();
}

}  // namespace uia
