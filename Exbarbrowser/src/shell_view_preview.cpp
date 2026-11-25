#include "shell_view.h"

#include "shell_ops.h"

#include "platform_win.h"

// ---------------------------------------------------------------- 预览窗格
//
// 【为什么独立成文件】预览窗格是 ShellView 里自成一体的一块：
//   · 状态是**每实例**的（成员 previewPaneWanted_），不能用文件级全局量 ——
//     GetPaneState 是每实例的 COM 方法，全局量会让"标签 A 关了预览窗格"波及所有标签；
//   · 观测面是"预览宿主窗口是否真的可见"（FOLDERFLAGS 里没有这一位）；
//   · 诊断段（-DEXBAR_DIAG_PREVIEW）重活多，且必须默认关闭。
// 这些与其他职责（导航、导航树、事件回放）没有共享状态，混在一个文件里只会
// 让"改预览窗格"和"改导航"互相踩脚。
namespace {

// 预览窗格意图是**每实例**状态（成员 previewPaneWanted_），不能用文件级全局量：
// GetPaneState 是每实例的 COM 方法，全局量会让"标签 A 关了预览窗格"波及所有标签。
// FOLDERFLAGS 里没有"显示预览窗格"这一位（0x400 是 FWF_NOSCROLL，几乎所有视图都置位），
// 真正决定窗格是否创建的是 IExplorerPaneVisibility::GetPaneState。

// 预览窗格宿主窗口：窗格隐藏时窗口对象仍然存在（被 resize 成 0 宽并 SW_HIDE），
// 所以必须同时看"可见 + 有实际宽度"，只按类名存在性判断会永远为真。
BOOL CALLBACK FindPreviewHostProc(HWND h, LPARAM lp) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, _countof(cls));
    if (wcscmp(cls, L"Shell Preview Extension Host") == 0) {
        RECT rc{};
        GetWindowRect(h, &rc);
        if (IsWindowVisible(h) && rc.right - rc.left > 0) {
            *reinterpret_cast<bool*>(lp) = true;
            return FALSE;
        }
    }
    return TRUE;
}

bool PreviewPaneVisibleNow(HWND container) {
    bool found = false;
    if (container) EnumChildWindows(container, FindPreviewHostProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

}  // namespace

void ShellView::CapturePreviewPaneState(IShellView*) {
    // 不能读 FOLDERFLAGS：其中没有"显示预览窗格"位（0x400 是 FWF_NOSCROLL，几乎总是置位，
    // 早先按它采样会把"已隐藏"误判成"已打开"，于是每次导航都被强制打开）。
    // 唯一可靠的观测是窗格宿主窗口是否真的可见。
    const bool visible = PreviewPaneVisibleNow(container_);
    previewPaneWanted_ = visible;
    previewPaneWantedKnown_ = true;
    LogDbg(L"[preview] capture visible=%d", visible ? 1 : 0);
}

void ShellView::ApplyPreviewPaneState(IShellView*) {
    // 窗格是否创建由 IExplorerPaneVisibility::GetPaneState 决定（见上面 Force 返回），
    // 框架每次建窗格都会来问，所以这里不再重导航去"抢修" —— 那会和 shell 自己的
    // 重建流程互相触发，表现为目录反复刷新。只做观测与日志，便于排查。
    if (!previewPaneWantedKnown_) return;
    const bool visible = PreviewPaneVisibleNow(container_);
    if (visible != previewPaneWanted_) {
        LogDbg(L"[preview] mismatch visible=%d wanted=%d（下一次建窗格按 GetPaneState 纠正）",
                visible ? 1 : 0, previewPaneWanted_ ? 1 : 0);
    }
}

void ShellView::CapturePreviewPaneBeforeNavigation() {
    IShellView* view = CurrentView();
    CapturePreviewPaneState(view);
    if (view) view->Release();
}

void ShellView::SchedulePreviewReapply() {
    if (!container_) return;
    KillTimer(container_, kPreviewReapplyTimer);
    SetTimer(container_, kPreviewReapplyTimer, 120, nullptr);
}

void ShellView::ReapplyPreviewPane() {
    // 真正的功能只有两行：停掉重试定时器、把预览窗格状态再套一次。
    // 原先这个函数里还塞着 ~160 行诊断（列枚举/逐项取大小/SetColumns 强制重建/UIA 树遍历）
    // —— 而它由 120ms 定时器反复调用，等于每次悬停预览都做一遍跨进程 UIA 与列重建。
    // 已抽到 DiagnosePreviewPaneState()，由 -DEXBAR_DIAG_PREVIEW 控制。
    if (container_) KillTimer(container_, kPreviewReapplyTimer);
    IShellView* view = CurrentView();
    ApplyPreviewPaneState(view);
    if (view) view->Release();
#if defined(EXBAR_DIAG_PREVIEW)
    DiagnosePreviewPaneState();
#endif
}

#if defined(EXBAR_DIAG_PREVIEW)
// 【诊断：预览窗格/文件夹大小】2026-11 排查"详细信息视图里 Size 列不显示"时写的。
//
// 为什么必须默认关闭：它原本直接长在 ReapplyPreviewPane 里，而后者由 120ms 定时器
// 反复调用。这段代码会做四件重活 ——
//   ① 在最多 40 个列里逐个 GetDetailsOf + StrRetToBufW 找 Size 列；
//   ② 对前 4 项取 PKEY_Size（网络路径/大文件夹 = 真实 I/O）；
//   ③ 用 IColumnManager 把 Size 列**移除再加回**（强制重建可见列，视图会闪）；
//   ④ UIA 全后代遍历（TreeScope_Descendants + 逐元素取名字，跨进程调用）。
// 留在生产路径上，用户每次把鼠标移过文件列表都要付一遍这个代价。
//
// 需要时加 -DEXBAR_DIAG_PREVIEW 重新启用。不要退化成注释 —— 注释掉的代码会腐烂，
// 活着但默认跑着的诊断更糟。
void ShellView::DiagnosePreviewPaneState() {
    IShellView* view = CurrentView();
    if (!view) return;
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);
    if (!fv2) {
        view->Release();
        return;
    }
    int n = 0;
    fv2->ItemCount(SVGIO_ALLVIEW, &n);
    LogDbg(L"[late] items=%d", n);

    // 【判空】GetFolder(IID_IShellFolder2) 在虚拟目录（搜索结果 / 库 / 第三方 NSE）
    // 上会失败。以前这里只把 hr 写进日志就继续用 sf2 —— 是必崩的空指针解引用，
    // 而本函数原来挂在 120ms 定时器上，等于"导航到某类目录就崩"。
    ComPtr<IShellFolder2> sf2;
    const HRESULT hsf = fv2->GetFolder(IID_IShellFolder2, reinterpret_cast<void**>(&sf2));
    LogDbg(L"[late] GetFolder(IShellFolder2) hr=0x%08X", static_cast<unsigned long>(hsf));
    if (FAILED(hsf) || !sf2) {
        view->Release();
        return;
    }

    int sizeCol = -1;
    for (int c = 0; c < 40; ++c) {
        SHELLDETAILS sd{};
        if (FAILED(sf2->GetDetailsOf(nullptr, c, &sd))) continue;
        wchar_t buf[128] = L"";
        if (SUCCEEDED(StrRetToBufW(&sd.str, nullptr, buf, _countof(buf))) &&
            (str::IEquals(buf, L"大小") || str::IEquals(buf, L"Size"))) {
            sizeCol = c;
            LogDbg(L"[late] Size column index=%d", c);
            break;
        }
    }
    for (int i = 0; i < n && i < 4; ++i) {
        PITEMID_CHILD pidl = nullptr;
        if (FAILED(fv2->Item(i, &pidl)) || !pidl) continue;
        VARIANT var{};
        HRESULT hdet = sf2->GetDetailsEx(pidl, &PKEY_Size, &var);
        unsigned long long v = 0;
        if (SUCCEEDED(hdet) && var.vt == VT_UI8) v = var.ullVal;
        if (SUCCEEDED(hdet)) VariantClear(&var);
        wchar_t detText[128] = L"";
        HRESULT hdetText = E_FAIL;
        if (sizeCol >= 0) {
            SHELLDETAILS sdItem{};
            hdetText = sf2->GetDetailsOf(pidl, sizeCol, &sdItem);
            if (SUCCEEDED(hdetText))
                StrRetToBufW(&sdItem.str, nullptr, detText, _countof(detText));
        }
        ComPtr<IShellItem> item;
        HRESULT hrs = SHCreateItemFromIDList(pidl, IID_IShellItem,
                                             reinterpret_cast<void**>(&item));
        CoTaskMemFree(pidl);
        if (FAILED(hrs) || !item) continue;
        PWSTR name = nullptr;
        item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
        SFGAOF attr = 0;
        item->GetAttributes(SFGAO_FOLDER, &attr);
        LogDbg(L"[late] item %d folder=%d details=%llu hr=0x%08X text=%s hrText=0x%08X name=%s", i,
               (attr & SFGAO_FOLDER) ? 1 : 0, v, static_cast<unsigned long>(hdet), detText,
               static_cast<unsigned long>(hdetText), name ? name : L"?");
        if (name) CoTaskMemFree(name);
    }

    // IColumnManager：把 Size 列移除再加回，强制 shell 重建可见列。
    ComPtr<IColumnManager> cm2;
    if (SUCCEEDED(fv2->QueryInterface(IID_IColumnManager, reinterpret_cast<void**>(&cm2))) && cm2) {
        UINT vc = 64;
        std::vector<PROPERTYKEY> vk(vc);
        HRESULT hget = cm2->GetColumns(CM_ENUM_VISIBLE, vk.data(), vc);
        int sizeIdx = -1;
        for (UINT k = 0; k < vc; ++k)
            if (IsEqualPropertyKey(vk[k], PKEY_Size)) sizeIdx = static_cast<int>(k);
        LogDbg(L"[late] toggle cols vc=%u get=0x%08X sizeIdx=%d", vc,
               static_cast<unsigned long>(hget), sizeIdx);
        if (sizeIdx >= 0 && vc > 1) {
            std::vector<PROPERTYKEY> without;
            for (UINT k = 0; k < vc; ++k)
                if (static_cast<int>(k) != sizeIdx) without.push_back(vk[k]);
            HRESULT h1 = cm2->SetColumns(without.data(), static_cast<UINT>(without.size()));
            HRESULT h2 = cm2->SetColumns(vk.data(), vc);
            LogDbg(L"[late] toggle hr1=0x%08X hr2=0x%08X", static_cast<unsigned long>(h1),
                   static_cast<unsigned long>(h2));
        }
    }

    HWND vh = nullptr;
    if (SUCCEEDED(view->GetWindow(&vh)) && vh) {
        ComPtr<IUIAutomation> uia;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IUIAutomation, reinterpret_cast<void**>(&uia))) &&
            uia) {
            ComPtr<IUIAutomationElement> root;
            if (SUCCEEDED(uia->ElementFromHandle(vh, &root)) && root) {
                VARIANT var{};
                var.vt = VT_I4;
                var.lVal = UIA_ListItemControlTypeId;
                ComPtr<IUIAutomationCondition> cond;
                if (SUCCEEDED(
                        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &cond)) &&
                    cond) {
                    ComPtr<IUIAutomationElementArray> arr;
                    if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond.get(), &arr)) &&
                        arr) {
                        int kc = 0;
                        arr->get_Length(&kc);
                        LogDbg(L"[uia] listitems=%d", kc);
                        for (int k = 0; k < kc; ++k) {
                            ComPtr<IUIAutomationElement> kid;
                            if (FAILED(arr->GetElement(k, &kid)) || !kid) continue;
                            BSTR kn = nullptr;
                            kid->get_CurrentName(&kn);
                            RECT kr{};
                            kid->get_CurrentBoundingRectangle(&kr);
                            LogDbg(L"[uia]   child %d name=%s rect=%d,%d,%d,%d", k,
                                   kn ? kn : L"?", kr.left, kr.top, kr.right, kr.bottom);
                            if (kn) SysFreeString(kn);
                        }
                    }
                }
            }
        }
    }
    view->Release();   // fv2 由 ComPtr 自动释放
}
#endif  // EXBAR_DIAG_PREVIEW


void ShellView::TogglePreviewPane() {
    // 【状态源】用"意图"而不是"此刻可见"。
    //
    // 原先以 PreviewPaneVisibleNow 为准取反，理由是 FOLDERFLAGS 没有预览窗格位。
    // 但预览窗格宿主在某些视图下**根本不会显示**（见上面 206 行记录的现象：
    // 配置为 true、宿主窗口已创建，实际仍不可见）。此时"可见"恒为 false，
    // 取反后永远是 true —— 表现为按 Alt+P / 调用命令毫无反应，且配置也不变，
    // 而命令照样返回成功（2026-11-21 结构化全量测试实测）。
    //
    // 现在的判据：真的可见 → 关；不可见 → 跟随上次意图（未知则用配置值）。
    // 这样"开→关→开"可靠，同时仍然尊重用户从 shell 自己的界面把窗格打开的情形。
    const bool visibleNow = PreviewPaneVisibleNow(container_);
    const bool current = visibleNow ||
                         (previewPaneWantedKnown_ ? previewPaneWanted_
                                                  : (panePrefs_.getShowPreview
                                                         ? panePrefs_.getShowPreview()
                                                         : false));
    previewPaneWanted_ = !current;
    previewPaneWantedKnown_ = true;
    LogDbg(L"[preview] toggle visible=%d current=%d -> wanted=%d", visibleNow ? 1 : 0,
            current ? 1 : 0, previewPaneWanted_ ? 1 : 0);

    IShellView* view = CurrentView();
    ApplyPreviewPaneState(view);
    if (view) view->Release();
    SchedulePreviewReapply();

    // 用户显式切换 = 改变偏好：经 App 注入的回调写回配置（R16：本层不碰 Config）。
    if (panePrefs_) panePrefs_.setPreviewPane(previewPaneWanted_);
}
