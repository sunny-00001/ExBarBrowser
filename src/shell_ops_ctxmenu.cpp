// shell 上下文菜单的弹出：对当前选中项（没有选中则弹背景菜单）。
//
// 【为什么需要自己弹】Shift+F10 是"键盘触发的上下文菜单"：系统把它转成
// WM_CONTEXTMENU 发给**焦点窗口**。文件列表区是 shell 的 DefView，它不处理
// 这个消息 —— DefWindowProc 一路往上转（DefView → shell 的内部容器 → 我们的
// 主窗口），而主窗口没有处理 WM_CONTEXTMENU，于是"文件列表区按 Shift+F10
// 没反应"。书签栏/标签栏有自己的窗口过程，所以它们有。
//
// 【标准路径】Explorer 自己也是这么做的，顺序不能变：
//   1. 选中项 → IShellFolder::GetUIObjectOf → IContextMenu
//      （没有选中项时取背景菜单 SVGIO_BACKGROUND）；
//   2. QueryContextMenu → 得到 HMENU；
//   3. TrackPopupMenuEx 让用户挑；
//   4. 挑中之后 InvokeCommand 执行。
//
// 【owner-draw 的坑】现代 shell 菜单是 **owner-draw** 的：绘制通过
// WM_MEASUREITEM / WM_DRAWITEM 发给 owner 窗口，宿主必须把它们转给
// IContextMenu2/3::HandleMenuMsg2，否则菜单项画不出来（一片空白）。
// 这里用 SetWindowSubclass 在**菜单跟踪期间**挂一个子类把这两个消息转出去，
// 跟踪结束就摘掉 —— 长期挂在窗口过程上会把"没有菜单"的时段也搅进去。
#include "shell_ops.h"

#include <commctrl.h>

#include "shell_ops_internal.h"

#include "modal.h"   // modal::Scope：登记模态层（AI 泵据此避让）

// 本机 SDK 的 shobjidl.h 里没有 GCSMENUVERB 这个常量名（值是 0，标准头里
// 与 GCSMENUVERBLONG 并列）。按标准值补上，别去猜别的。
#ifndef GCSMENUVERB
#define GCSMENUVERB 0
#endif

namespace shellops {

namespace {

// 菜单跟踪期间的窗口子类：把 owner-draw 消息转给 IContextMenu3 / IContextMenu2。
//
// 【为什么必须转发】shell 的菜单项是 MFT_OWNERDRAW，绘制信息在 IContextMenu3 里，
// 不转发 = owner 收到 WM_DRAWITEM 却画不出内容（菜单一片空白）。
struct MenuSubclass {
    IContextMenu3* cm3 = nullptr;   // 优先：多一层 WM_INITMENUPOPUP 的动态项处理
    IContextMenu2* cm2 = nullptr;   // 退回：老一点的 shell 不给 IContextMenu3
};

LRESULT CALLBACK MenuOwnerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                               UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MenuSubclass*>(ref);
    // 这三个消息是 shell 菜单的通道，必须原样转交：
    //   · WM_MEASUREITEM / WM_DRAWITEM —— owner-draw 的测量与绘制；
    //   · WM_INITMENUPOPUP —— 菜单弹出时的**动态项**（"压缩为…"、云盘扩展项等）
    //     在这一阶段才被加进来，不转发就会出现"菜单少几项"。
    // （WM_MENUCHAR 不转：菜单跟踪期的键盘导航由 TrackPopupMenu 自己处理。）
    if (self && (msg == WM_MEASUREITEM || msg == WM_DRAWITEM ||
                 msg == WM_INITMENUPOPUP)) {
        LRESULT r = 0;
        // 【本机 SDK 的事实】IContextMenu2 只有 HandleMenuMsg（无 plResult 出参）；
        // HandleMenuMsg2 在 IContextMenu3 上。cm2 分支拿不到返回值，按 owner-draw
        // 消息的惯例回 0 —— 绘制照样成立，只是"菜单自己要返回特定值"的场景没有。
        HRESULT hr = E_FAIL;
        if (self->cm3)
            hr = self->cm3->HandleMenuMsg2(msg, wp, lp, &r);
        else if (self->cm2)
            hr = self->cm2->HandleMenuMsg(msg, wp, lp);
        if (SUCCEEDED(hr)) return r;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// 取当前选中项的子 PIDL 列表（多选时是全部）。
//
// 【为什么用 GetSelection 而不是 GetSelectionMarkedItem】Shift 多选时标记项只代表
// 焦点项，改名/属性会只作用一个；复选模式下甚至没有标记项 → 命令静默失败。
bool SelectedChildPidls(IFolderView2* fv2, std::vector<PCUITEMID_CHILD>* out) {
    ComPtr<IShellItemArray> sel;
    if (FAILED(fv2->GetSelection(FALSE, &sel)) || !sel) return false;
    DWORD n = 0;
    sel->GetCount(&n);
    if (!n) return false;
    out->reserve(static_cast<size_t>(n));
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(sel->GetItemAt(i, &item)) || !item) continue;
        PIDLIST_ABSOLUTE full = nullptr;
        if (FAILED(SHGetIDListFromObject(item.get(), &full)) || !full) continue;
        // ILCloneChild 可能失败（内存不足）→ 跳过，别把空 PIDL 塞进
        // GetUIObjectOf 的数组里（那会让整条命令失败）。
        PCUITEMID_CHILD child = ILCloneChild(ILFindLastID(full));
        CoTaskMemFree(full);
        if (!child) continue;
        out->push_back(child);
    }
    return !out->empty();
}

}  // namespace

bool ShowContextMenu(IShellView* view, HWND owner, POINT screen) {
    ComPtr<IFolderView2> fv2 = detail::GetFv2(view);
    if (!fv2) return false;

    ComPtr<IContextMenu> cm;
    std::vector<PCUITEMID_CHILD> pidls;
    ComPtr<IShellFolder> folder = detail::GetFolder(fv2.get());
    if (folder && SelectedChildPidls(fv2.get(), &pidls)) {
        folder->GetUIObjectOf(owner, static_cast<UINT>(pidls.size()), pidls.data(),
                              IID_IContextMenu, nullptr, reinterpret_cast<void**>(&cm));
    }
    for (auto* p : pidls) CoTaskMemFree(const_cast<PITEMID_CHILD>(p));

    // 没有选中项（或取不到选中项菜单）→ 背景菜单（新建/粘贴/查看/排序…）。
    // 背景菜单要走 **IShellView::GetItemObject**（IFolderView2 没有这个方法）。
    if (!cm) {
        view->GetItemObject(SVGIO_BACKGROUND, IID_IContextMenu,
                            reinterpret_cast<void**>(&cm));
    }
    if (!cm) return false;

    // 菜单项 id 段：与 shell_ops.cpp 的 Undo 那条路**同一口径**（1..0x6FFF，
    // 在 FCIDM_SHVIEWFIRST..LAST 之内，避免与全局菜单冲突）。
    //
    // 【hmenu 是输入参数，不是输出参数】QueryContextMenu 的契约是"把 shell 的
    // 菜单项**插进调用方提供的这个菜单里**"，它不会自己建一个菜单返回给你。
    // 传 nullptr = shell 没有地方插项 → 菜单里一项都没有 → TrackPopupMenuEx
    // 弹出一个空菜单（甚至直接失败）。
    //
    // 【idCmdFirst..idCmdLast 是区间】两端相等 = 区间为空 = shell 一项都不插。
    // 必须留出宽度（shell 菜单动辄上百项）。
    HMENU hmenu = CreatePopupMenu();
    if (!hmenu) return false;
    const HRESULT hr = cm->QueryContextMenu(hmenu, 0, 1, 0x6FFF, CMF_NORMAL);
    if (FAILED(hr)) {
        DestroyMenu(hmenu);
        return false;
    }

    // owner-draw 的转发通道（见文件头"owner-draw 的坑"）。
    // 【三者都必须活到 RemoveWindowSubclass 之后】子类挂着的时候它们不能析构：
    //   · cm3 —— IContextMenu3 的实现；
    //   · cm2 —— 老 shell 的回退实现。**必须与本块的 cm3 同作用域**：以前把它
    //     声明在 else 块里，出了块就 Release（引用计数归零即销毁），而 mc.cm2
    //     还指着它 —— 菜单跟踪期间一收到 WM_DRAWITEM 就是 use-after-free
    //     （任何不提供 IContextMenu3 的 shell 扩展都会触发）；
    //   · mc  —— 子类数据本身。
    ComPtr<IContextMenu3> cm3;
    ComPtr<IContextMenu2> cm2;
    MenuSubclass mc;
    if (SUCCEEDED(cm->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&cm3))) && cm3) {
        mc.cm3 = cm3.get();
    } else {
        // 退回 IContextMenu2：HandleMenuMsg（不带 plResult 出参）是 IContextMenu2
        // 就有的方法；老一点的 shell 不给 IContextMenu3，但 owner-draw 转发仍要成立。
        if (SUCCEEDED(cm->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&cm2))) && cm2)
            mc.cm2 = cm2.get();
    }

    SetWindowSubclass(owner, MenuOwnerProc, 1, reinterpret_cast<DWORD_PTR>(&mc));

    // TPM_MENUKEY / CMF_MASK_HMENU 这两样在**本机 SDK 的 shobjidl.h 里没有**
    // （它是精简过的头文件），所以：
    //   · 不用 TPM_MENUKEY（那是给"菜单键也能开菜单"用的，我们自己弹，不需要）；
    //   · 不用 CMF_MASK_HMENU + lpVerb=MAKEINTRESOURCE(id) 那条路
    //     （CMINVOKECOMMANDINFOEX 里根本没有 hMenu 字段可填），
    //     改走**助记符**：fMask=0 时 shell 只读前 7 个字段（布局与真实 SDK 一致），
    //     lpVerb 必须是 ANSI 字符串。
    // modal::Scope：菜单跟踪是一段**吞消息的模态循环**，期间必须让 AI 泵避让。
    // 不登记的话，模态期间 WM_TIMER 照常派发 → ai::PumpRequests() 执行命令 →
    // 比如 bmk.* 重建书签栏 / tab.close 销毁视图 → 我们手里这些 PIDL、IContextMenu、
    // 甚至 owner 窗口都可能已经在菜单还开着的时候被换掉或销毁。
    // 与 popup_list.cpp 的 ShowPopupList 同一口径（那里也是先登记再跟踪）。
    modal::Scope modalScope;
    const int cmd = static_cast<int>(TrackPopupMenuEx(
        hmenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, owner,
        nullptr));

    RemoveWindowSubclass(owner, MenuOwnerProc, 1);
    // 菜单是我们建的，必须自己销毁（shell 只往里插项，不负责收）。
    DestroyMenu(hmenu);

    // 【取消时返回 0，不是负数】TPM_RETURNCMD 的契约：用户取消/菜单没选中任何项
    // 时返回 **0**。以前写的是 cmd < 0，于是按 Esc 会被当成"选中了 id 0"，
    // 继续往下走用假 id 去取动词、去 InvokeCommand —— 配合下面 GetCommandString
    // 的参数错位，表现就是"取消菜单反而执行了第一项（通常是打开）"。
    // 有效 id 从 idCmdFirst=1 起，所以 <= 0 一律是取消。
    if (cmd <= 0) return false;

    // 取该项的助记符（"open" / "rename" / "delete"…）：InvokeCommand 要的是它。
    // 取不到就不执行 —— fMask=0 时 lpVerb 会被当成 ANSI 字符串读，
    // 硬塞 MAKEINTRESOURCEA(cmd) 等于让 shell 去读一个假指针。
    //
    // 【为什么必须走 GetCommandString】本机 SDK 的 CMINVOKECOMMANDINFOEX
    // **没有 hMenu 字段**，于是 CMIF_MASK_HMENU + MAKEINTRESOURCE(id) 那条
    // 更直接的路走不通 —— GetCommandString 取动词是唯一可行的标准做法
    // （与 MSDN 的 IContextMenu2 "Creating a Shortcut Menu" 示例同路）。
    //
    // 【参数顺序与 id 偏移，两个都错过一次】
    //   签名是 (idCmd, uType, pReserved, pszName, cchMax)。
    //   · 以前把 GCSMENUVERB 和 cmd 写反了：idCmd 传了 0、uType 传了菜单 id。
    //     后果不只是"取不到"：uType=4 是 GCS_VERBW，shell 会把 **UTF-16**
    //     写进 char[128]（按字符数算 = 最多 256 字节，栈越界）；uType=5 是
    //     GCS_HELPTEXTW，拿到的是帮助文本当动词用；其余 id 直接 E_INVALIDARG
    //     → 全部 return false → **菜单弹得出来、点任何项都没反应**。
    //   · idCmd 必须减掉 idCmdFirst（本函数用 1，见上面的 QueryContextMenu）：
    //     菜单 id 是绝对 id，GetCommandString 要的是相对偏移。同仓
    //     shell_ops.cpp 的 MAKEINTRESOURCEA(id - 1) 就是这个口径 —— 直接送
    //     cmd 会拿到**相邻那一项**的动词，执行错的命令。
    //
    // 【已知风险，见 shell_ops.cpp 的实测记录】GetCommandString 会为菜单项
    // **懒加载对应的 shell 扩展**；第三方扩展（云盘客户端/右键增强）若在其
    // 处理里阻塞，这里就回不来，而本函数在 UI 线程 —— 整个消息泵会卡住。
    // 与 shell_ops.cpp 那条路的差别有两条，且都是缓解：
    //   ① 这里**只问被选中的那一项**（那边要遍历上百项逐个问）；
    //   ② 这条路是**人工触发**（Shift+F10），不在 AI 通道的定时器里。
    // 一旦实测遇到卡顿，换的路是：改用 CMINVOKECOMMANDINFO + hMenu 语义
    // （需要自己拼一个带 hMenu 的结构体），而不是退回逐项遍历。
    char verb[128] = {};
    const HRESULT hrVerb = cm->GetCommandString(static_cast<UINT_PTR>(cmd - 1), GCSMENUVERB,
                                               nullptr, verb, sizeof(verb));
    if (FAILED(hrVerb) || !verb[0]) {
        LogDbg(L"[shellops] 上下文菜单项 %d 取不到助记符，跳过执行", cmd);
        return false;
    }

    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = 0;
    info.hwnd = owner;
    info.lpVerb = verb;
    info.lpVerbW = nullptr;
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hrInvoke =
        cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    return SUCCEEDED(hrInvoke);
}

}  // namespace shellops
