// 导航树的事件适配器：INameSpaceTreeControlEvents 的实现。
//
// 【为什么单独成文件】这里全是 COM 契约与"事件语义"（哪个回调该导航、哪个只
// 记录、返回值怎么影响控件默认行为），与 nav_pane.cpp 的"控件生命周期"
// （建窗口/缩放/套主题）是两套完全不同的改动理由。而且它是本工程里唯一
// 需要精确背诵官方 Remarks 的地方，值得单独放，方便对照文档复核。
#include "nav_pane.h"

#include "log.h"

#include <commctrl.h>
#include <new>

namespace navpane {

namespace {
// NSTC 在某些 Windows 版本上对所有树右键都把 psi 传成 null（真实点中项也是），
// 只靠 psi 判断"空白区"会把选中项的系统菜单也替换掉。内部 SysTreeView32 的
// 命中测试是同一时刻最可靠的判据：点中任何树节点都交回 NSTC，只有真正落在
// 行外的空白区才进入下面的“空菜单 + 延后显示”路径。
bool CursorOverNavItem(NavPane* owner) {
    HWND tv = owner->treeWindow();
    if (!tv) return false;
    POINT screen{};
    if (!GetCursorPos(&screen)) return false;
    if (!ScreenToClient(tv, &screen)) return false;
    TVHITTESTINFO hit{};
    hit.pt = screen;
    const HTREEITEM node = reinterpret_cast<HTREEITEM>(
        SendMessageW(tv, TVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit)));
    return node != nullptr && (hit.flags & TVHT_ONITEM) != 0;
}

// 给 NSTC 的占位 IContextMenu：不往原生菜单里插任何项，只在 QueryContextMenu
// 时把真正的弹层投给宿主窗口。这样 NSTC 自己的事件/跟踪流程照常结束，原生菜单
// 窗口不出现；随后 ui::Menu::Show 在与书签栏右键完全相同的弹层里显示菜单。
//
// 【为什么不在 OnBeforeContextMenu 里直接 Show】那是 shell 的事件回调栈，弹层
// 自己的模态循环会重入 NSTC 的消息状态，之前实测卡死。PostMessage 把显示时机
// 移到正常消息循环，既避开重入，也保住 NSTC 对“是否替换菜单”的契约。
class DeferredBlankMenu final : public IContextMenu3 {
public:
    explicit DeferredBlankMenu(HWND host) : host_(host) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IContextMenu || riid == IID_IUnknown) {
            *ppv = static_cast<IContextMenu*>(this);
        } else if (riid == IID_IContextMenu2) {
            *ppv = static_cast<IContextMenu2*>(this);
        } else if (riid == IID_IContextMenu3) {
            *ppv = static_cast<IContextMenu3*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override {
        if (!posted_ && IsWindow(host_)) {
            POINT screen{};
            if (GetCursorPos(&screen)) {
                PostMessageW(host_, WM_APP_NAV_BLANK_MENU, 0,
                             MAKELPARAM(screen.x, screen.y));
                posted_ = true;
            }
        }
        // 一项都不交回 shell：NSTC 不会得到一个有内容的原生菜单可跟踪。
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(LPCMINVOKECOMMANDINFO) override {
        return E_INVALIDARG;   // 没有原生菜单项，正常路径不会走到这里
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT, WPARAM, LPARAM) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT, WPARAM, LPARAM, LRESULT*) override {
        return S_FALSE;
    }

private:
    ~DeferredBlankMenu() = default;

    LONG ref_ = 1;
    HWND host_ = nullptr;
    bool posted_ = false;
};
}  // namespace

// INameSpaceTreeControlEvents 适配器。
//
// 【关键：OnGetDefaultIconIndex 必须"不表态"】
// 探针 41 的 2×2×2 矩阵结论：
//   本回调返回 E_NOTIMPL  -> 16/16 个节点都有图标（首根 iImage=54）
//   本回调返回 S_OK 且写 -1 -> **0/16** 个节点有图标（首根 iImage=-1）
// NSTC 只在"宿主不表态"时才用 shell 自己算的图标下标；一旦宿主回了任何值
// （哪怕 -1），NSTC 就当**真下标**塞给节点，-1 超界 = 不画图标 = 整树空白。
// 旧实现返回 S_OK 却没写输出参数，则退到索引 0 = 每行都是空白图。
class NavPane::Events final : public INameSpaceTreeControlEvents {
public:
    explicit Events(NavPane* owner) : owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_INameSpaceTreeControlEvents) {
            *ppv = static_cast<INameSpaceTreeControlEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // 【选中变化**不能**用来导航】实测（干净 profile、无会话数据）：
    //   建树后 NSTC 自己连抛三次选中变化 —— 图库 → 此电脑 → 网络；若接成导航，
    //   一启动活动窗格就被拽到"网络"。SyncTo（切窗格时高亮回灌）也会触发它，
    //   于是形成"视图 -> 树 -> 视图"的自激回环。
    // NSTC 的 OnSelectionChanged **不区分来源**（用户 vs 程序化 AppendRoot /
    // SetItemState / 首次布局），这是控件契约的缺口，抑制标志也堵不住 ——
    // 事件可能在消息循环里异步到达，那时标志已退出。
    //
    // 所以这里**只记录**高亮位置，不导航；导航一律走 OnItemClick / 键盘回车。
    // 返回值按文档口径就是 S_OK（本回调没有"屏蔽默认处理"的语义）。
    HRESULT STDMETHODCALLTYPE OnSelectionChanged(IShellItemArray* items) override {
        if (!items || !owner_) return S_OK;
        DWORD count = 0;
        if (FAILED(items->GetCount(&count)) || count == 0) return S_OK;
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(0, &item)) && item) {
            owner_->NoteSelection(item);   // 只更新"树当前停在哪儿"
            item->Release();
        }
        return S_OK;
    }

    // 【返回值必须按分支分开选 —— 官方 Remarks】
    //   "If this method fails, the event is processed by both INameSpaceTreeControl
    //    and TreeView. If it returns S_OK, then only INameSpaceTreeControl will
    //    process the event."
    //   · 标签/图标（我们自己导航）→ S_OK：NSTC 独占，避免 TreeView 再按
    //     NSTCS_SINGLECLICKEXPAND 处理一遍（一次点击两套动作）。
    //   · 展开箭头（+/-）→ S_FALSE：折叠是 TreeView 的默认动作，
    //     S_OK 会把它屏蔽，实机症状正是"只能展开、永远折不回去"。
    //
    // clickType 是 NSTCECLICKTYPE，mingw 头里只有 LBUTTON/MBUTTON/RBUTTON/
    // BUTTON/DBLCLICK 五个值，**没有 KEYDOWN** —— 键盘走 OnKeyboardInput。
    HRESULT STDMETHODCALLTYPE OnItemClick(IShellItem* item, NSTCEHITTEST hitTest,
                                          NSTCECLICKTYPE clickType) override {
        if (!item || !owner_) return S_OK;
        // 【建树期间一律忽略】"启动总是跳到网络"的正解。
        // 实测：AppendRoot(NSTCRS_EXPANDED) 展开某项时，NSTC **在调用栈内同步**
        // 回调 OnItemClick（hit=ONITEMLABEL、type=LBUTTON），路径就是那个根；
        // 一次建树连抛四次，活动窗格被拽到网络。因为同步，作用域标志能精确覆盖。
        //
        // **不用 GetAsyncKeyState 做判据**：实机日志证明它会把真实点击也吞掉
        // （三次真实点击 hit=4/32、type=0x1 全被误判成"非真实点击"，
        // 于是"点文件夹、文件列表不动"）。判据只能落在我们自己可控的窗口上。
        if (owner_->suppressClick_) return S_OK;
        // 右键/中键要弹上下文菜单 —— 交回控件。
        if (!ISLBUTTON(clickType)) return S_FALSE;

        // 【诊断】每次真实点击留一行：命中区 + 解析路径（含 shell: 前缀归一化）。
        // 判"点了没反应"：有本行 + 紧跟 [nav] request -> 导航已发起；
        // 有本行但无 [nav] -> 路径被解析成空/非法；连本行都没有 -> 回调没进来。
        LogDbg(L"[navpane] click hit=%d type=0x%X path=%s", static_cast<int>(hitTest),
               static_cast<unsigned>(clickType), PathOfItem(item).c_str());

        // 点展开箭头（+/-）只切展开/折叠，不改当前目录 —— explorer 就是这个行为。
        // **必须 S_FALSE**：折叠是 TreeView 默认动作，S_OK 会屏蔽它。
        if (hitTest & NSTCEHT_ONITEMBUTTON) return S_FALSE;

        // 真实点击：导航到该项，然后 S_OK 让 NSTC 独占这次点击。
        owner_->OnPicked(item);
        return S_OK;
    }

    // 键盘：回车 = "进入当前选中项"。
    //
    // 【返回值语义与 OnItemClick 相反，别照抄】官方 Remarks：
    //   "When this returns S_OK, the message was **not consumed** and the
    //    namespace tree control is allowed to process the message. Otherwise
    //    this message was handled, with no further action required."
    // 即 OnKeyboardInput 的 S_OK = 交回控件，S_FALSE = 我们已消费。
    // 所以"回车且确实有高亮项"才回 S_FALSE（导航已做完，控件别再展开一次），
    // 其余一律 S_OK，让方向键/翻页/展开等行为原样保留。
    HRESULT STDMETHODCALLTYPE OnKeyboardInput(UINT uMsg, WPARAM wParam, LPARAM) override {
        if (uMsg != WM_KEYDOWN || wParam != VK_RETURN || !owner_) return S_OK;
        return owner_->PickCurrent() ? S_FALSE : S_OK;
    }

    // ---- 图标索引：必须"不表态"，否则整棵树一个图标都画不出来 ----
    // 只返回 E_NOTIMPL，**绝不碰输出参数**（见本类顶部那段探针 41 结论）。
    HRESULT STDMETHODCALLTYPE OnGetDefaultIconIndex(IShellItem*, int*, int*) override {
        return E_NOTIMPL;
    }

    // ---- 其余回调：通知类，S_OK 表示"允许继续"，不改变控件行为 ----
    HRESULT STDMETHODCALLTYPE OnPropertyItemCommit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemStateChanging(IShellItem*, NSTCITEMSTATE,
                                                   NSTCITEMSTATE) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemStateChanged(IShellItem*, NSTCITEMSTATE,
                                                  NSTCITEMSTATE) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnBeforeExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAfterExpand(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeginLabelEdit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndLabelEdit(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGetToolTip(IShellItem*, LPWSTR, int) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnBeforeItemDelete(IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemAdded(IShellItem*, WINBOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemDeleted(IShellItem*, WINBOOL) override { return S_OK; }
    // 【右键菜单必须"不表态"或"交出 IContextMenu"，不能返回空 S_OK】
    // 这两个回调的 riid/ppv 是给宿主**替换菜单**用的：系统只看 HRESULT，
    // 不会检查 ppv 是否真的写了指针。返回 S_OK 却没填 ppv，NSTC 随后会
    // 解引用空指针 —— 实机症状就是"右键选中项/空白区后卡死"。
    // 选中项：交回 NSTC/shell 用默认 IContextMenu（Pin/打开/属性都有）。
    // 空白区：NSTCS2_SHOWNULLSPACEMENU 打开后 psi 为 null；再经 TreeView 命中
    // 测试确认不在行上，交出 DeferredBlankMenu。它不产生原生菜单项，只把真正的
    // 弹层投到宿主消息循环，走书签栏同一条 ui::Menu::Show 路径。
    HRESULT STDMETHODCALLTYPE OnBeforeContextMenu(IShellItem* psi, REFIID riid,
                                                   void** ppv) override {
        if (psi || !owner_ || !ppv) return E_NOTIMPL;
        if (CursorOverNavItem(owner_)) return E_NOTIMPL;
        DeferredBlankMenu* menu = new (std::nothrow) DeferredBlankMenu(owner_->host_);
        if (!menu) return E_OUTOFMEMORY;
        // QueryInterface 负责按 riid 写出 ppv；不支持的 riid 原样失败，
        // 让 NSTC 退回自己的默认处理，绝不留下未初始化的 ppv。
        const HRESULT hr = menu->QueryInterface(riid, ppv);
        menu->Release();
        LogDbg(L"[navpane] 空白区右键：延后到 ui::Menu::Show，hr=0x%08lX",
               static_cast<unsigned long>(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID,
                                                  void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OnBeforeStateImageChange(IShellItem*) override { return S_OK; }

private:
    LONG ref_ = 1;
    NavPane* owner_;
};

// 工厂：nav_pane.cpp 只把它当 IUnknown 用，不需要 Events 的完整类型。
INameSpaceTreeControlEvents* NavPane::MakeEvents(NavPane* owner) { return new Events(owner); }

}  // namespace navpane