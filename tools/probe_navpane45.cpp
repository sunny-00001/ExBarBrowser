// 探针 45：只读导出**运行中 Explorer** 的导航窗格树（MSAA / oleacc）。
//
// 目的：拿到这台机器上 Explorer 左栏的权威结构（文本 + 视觉顺序 + 缩进 + 层级），
//       作为"我方 NSTC 树"的比对基线。
// 为什么走 MSAA：Explorer 的导航窗格是 ProperTreeHost 里的普通 SysTreeView32，
//       UIA 能看到树控件本身但枚举不到子项（shell 只给了 MSAA 代理）。
// 合规：全程只读，不发任何合成输入（无 SendInput/mouse_event/SetCursorPos/
//       合成窗口消息），符合工程规则 R21。
#include <windows.h>
#include <oleacc.h>
#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

std::string U8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    r.resize(static_cast<size_t>(n - 1));
    return r;
}

struct Item {
    std::wstring name;
    int x = 0, y = 0, w = 0, h = 0;
    int state = 0;
    int childCount = 0;
    std::string parent = "";
    std::vector<Item> children;
};

HWND FindExplorerTree() {
    HWND explorer = FindWindowW(L"CabinetWClass", nullptr);
    if (!explorer) return nullptr;
    struct Ctx {
        HWND found = nullptr;
    } ctx;
    EnumChildWindows(explorer, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"SysTreeView32") == 0) {
            c->found = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 找"标题里含关键字的窗口"下面的 SysTreeView32（用于 dump 本项目自己的应用）
HWND FindTreeByTitle(const wchar_t* titleLike) {
    struct Ctx {
        const wchar_t* want;
        HWND found = nullptr;
    } ctx{titleLike, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t title[512] = L"";
        GetWindowTextW(h, title, 512);
        if (!title[0] || !wcsstr(title, c->want)) return TRUE;
        struct Inner {
            HWND found = nullptr;
        } inner;
        EnumChildWindows(h, [](HWND ch, LPARAM ilp) -> BOOL {
            auto* ic = reinterpret_cast<Inner*>(ilp);
            wchar_t cls[64] = L"";
            GetClassNameW(ch, cls, 64);
            if (wcscmp(cls, L"SysTreeView32") == 0) {
                ic->found = ch;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&inner));
        if (inner.found) {
            c->found = inner.found;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::wstring AccName(IAccessible* a, VARIANT id) {
    if (!a) return L"";
    BSTR b = nullptr;
    if (FAILED(a->get_accName(id, &b)) || !b) return L"";
    std::wstring s = b;
    SysFreeString(b);
    return s;
}

int AccState(IAccessible* a, VARIANT id) {
    if (!a) return 0;
    VARIANT v{};
    if (FAILED(a->get_accState(id, &v))) return 0;
    int s = v.lVal;
    VariantClear(&v);
    return s;
}

void Loc(IAccessible* a, VARIANT id, int* x, int* y, int* w, int* h) {
    *x = *y = *w = *h = 0;
    if (!a) return;
    LONG l = 0, t = 0, cw = 0, ch = 0;
    if (FAILED(a->accLocation(&l, &t, &cw, &ch, id))) return;
    *x = l;
    *y = t;
    *w = cw;
    *h = ch;
}

// 枚举容器的子项：AccessibleChildren 返回 VARIANT 数组，
// 每个元素要么是 VT_I4（简单子项 id），要么是 VT_DISPATCH（复杂子项对象）。
bool Children(IAccessible* container, std::vector<VARIANT>* out) {
    out->clear();
    LONG count = 0;
    if (FAILED(container->get_accChildCount(&count)) || count <= 0) return false;
    std::vector<VARIANT> buf(static_cast<size_t>(count));
    for (auto& v : buf) VariantInit(&v);
    LONG obtained = 0;
    HRESULT hr = AccessibleChildren(container, 0, count, buf.data(), &obtained);
    if (FAILED(hr) || obtained <= 0) return false;
    buf.resize(static_cast<size_t>(obtained));
    for (auto& v : buf) out->push_back(v);
    return true;
}

Item Build(IAccessible* container, VARIANT child) {
    Item it;
    IAccessible* node = nullptr;
    if (child.vt == VT_DISPATCH && child.pdispVal) {
        node = static_cast<IAccessible*>(child.pdispVal);
    }
    if (node) {
        VARIANT self{};
        V_VT(&self) = VT_I4;
        self.lVal = 0;  // CHILDID_SELF
        it.name = AccName(node, self);
        it.state = AccState(node, self);
        Loc(node, self, &it.x, &it.y, &it.w, &it.h);
        // 【层级判定】accParent 指向谁？accChildCount 有几个孩子？
        // 父若是树根 => 顶层兄弟；父是别的项 => 那项的孩子。
        VARIANT par{};
        V_VT(&par) = VT_I4;
        par.lVal = 0;
        IDispatch* pdisp = nullptr;
        if (SUCCEEDED(node->get_accParent(&pdisp)) && pdisp) {
            IAccessible* p = static_cast<IAccessible*>(pdisp);
            VARIANT pself{};
            V_VT(&pself) = VT_I4;
            pself.lVal = 0;
            std::wstring pn = AccName(p, pself);
            it.parent = U8(pn.c_str());
            p->Release();
            pdisp->Release();
        } else {
            it.parent = "(none)";
        }
        LONG cc = 0;
        node->get_accChildCount(&cc);
        it.childCount = cc;
        std::vector<VARIANT> kids;
        if (Children(node, &kids)) {
            for (auto& k : kids) it.children.push_back(Build(node, k));
        }
    } else {
        // 简单子项：名字/坐标从容器对象取
        it.name = AccName(container, child);
        it.state = AccState(container, child);
        Loc(container, child, &it.x, &it.y, &it.w, &it.h);
        it.parent = "<simple>";
        it.childCount = 0;
    }
    return it;
}

void SortByY(std::vector<Item>* v) {
    std::sort(v->begin(), v->end(), [](const Item& a, const Item& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    for (auto& c : *v) SortByY(&c.children);
}

void Print(const std::vector<Item>& items, int level) {
    for (const auto& it : items) {
        std::string f;
        // accState 位：0x200=EXPANDED 0x400=COLLAPSED 0x8000=INVISIBLE 0x10000=OFFSCREEN
        if (it.state & 0x200) f += "EXPANDED ";
        if (it.state & 0x400) f += "COLLAPSED ";
        if (it.state & 0x8000) f += "INVISIBLE ";
        if (it.state & 0x10000) f += "OFFSCREEN ";
        printf("%*s- %-42s [x=%-4d y=%-5d kids=%-3d parent=%-14s %s]\n", level * 2, "",
               U8(it.name.c_str()).c_str(), it.x, it.y, it.childCount, it.parent.c_str(),
               f.c_str());
        if (!it.children.empty()) Print(it.children, level + 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // 用法：probe45.exe            -> dump Explorer 的导航树
    //       probe45.exe <标题关键字> -> dump 标题含关键字的窗口下的树（用于本项目自己）
    HWND tree = nullptr;
    if (argc >= 2) {
        wchar_t want[256] = L"";
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, want, 256);
        tree = FindTreeByTitle(want);
    } else {
        tree = FindExplorerTree();
    }
    if (!tree) {
        printf("NO_NAV_TREE\n");
        return 1;
    }
    wchar_t title[256] = L"";
    GetWindowTextW(tree, title, 256);
    printf("NAV_TREE hwnd=0x%llX title=[%s]\n", (unsigned long long)tree, U8(title).c_str());

    IAccessible* acc = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(tree, OBJID_CLIENT, IID_IAccessible,
                                            reinterpret_cast<void**>(&acc));
    printf("AccessibleObjectFromWindow hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !acc) return 2;

    std::vector<VARIANT> kids;
    if (!Children(acc, &kids)) {
        printf("NO_CHILDREN\n");
        return 3;
    }
    printf("TOP_LEVEL_COUNT: %zu\n", kids.size());

    std::vector<Item> roots;
    for (auto& k : kids) roots.push_back(Build(acc, k));
    SortByY(&roots);
    Print(roots, 0);

    acc->Release();
    return 0;
}
