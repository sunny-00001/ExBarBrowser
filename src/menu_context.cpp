// 统一菜单组件的 Shell 上下文菜单桥接实现。见 menu_context.h。
#include "menu_context.h"

#include "popup_list_draw.h"

#include <algorithm>

namespace ui {
namespace {

// 原生菜单按 MENUITEMINFO 的字符串做助记符定位，PopupDrawItem 则用 DT_NOPREFIX
// 直接落字。显示副本只去掉单 &（"&&" 保留一个字面 &），避免画出 "刷新(&R)"。
std::wstring PlainLabel(const std::wstring& label) {
    std::wstring out;
    out.reserve(label.size());
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == L'&') {
            if (i + 1 < label.size() && label[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
            }
            continue;
        }
        out.push_back(label[i]);
    }
    return out;
}

}  // namespace

ShellContextMenu::ShellContextMenu(const Menu& menu, const Theme& theme, InvokeFn invoke)
    : theme_(&theme), menu_(menu), invoke_(std::move(invoke)) {}

ShellContextMenu::~ShellContextMenu() = default;

HRESULT STDMETHODCALLTYPE ShellContextMenu::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IContextMenu) {
        *ppv = static_cast<IContextMenu*>(this);
    } else if (riid == IID_IContextMenu2) {
        *ppv = static_cast<IContextMenu2*>(this);
    } else if (riid == IID_IContextMenu3) {
        *ppv = static_cast<IContextMenu3*>(this);
    } else if (riid == IID_IUnknown) {
        *ppv = static_cast<IContextMenu*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ShellContextMenu::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

ULONG STDMETHODCALLTYPE ShellContextMenu::Release() {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

HRESULT STDMETHODCALLTYPE ShellContextMenu::QueryContextMenu(HMENU hmenu, UINT indexMenu,
                                                            UINT idCmdFirst, UINT idCmdLast,
                                                            UINT uFlags) {
    if (!hmenu) return E_INVALIDARG;
    if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);

    // 【自动快捷键的接线点】每次弹出都现取键位表。调用点只写了 .Cmd(CmdId)，
    // 以后给那条命令加键，这里下一次就会把新文案画进菜单右端。
    rows_ = menu_.rows();
    menu_.FillShortcuts(&rows_);
    // 显示副本走与书签菜单完全相同的渲染器；原 rows_ 留给 MENUITEMINFO 的
    // 助记符串与动作回传，避免把"系统用来定位的文案"改成无 & 版本。
    drawRows_ = rows_;
    for (PopupRow& row : drawRows_) row.label = PlainLabel(row.label);

    if (rows_.empty() || rows_.size() > 0xFFFF) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    }
    const UINT count = static_cast<UINT>(rows_.size());
    if (idCmdLast < idCmdFirst + count) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    }

    for (UINT i = 0; i < count; ++i) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_DATA;
        item.fState = rows_[i].disabled ? MFS_DISABLED : MFS_ENABLED;
        item.wID = idCmdFirst + i;
        item.dwItemData = i;  // MEASUREITEM/DRAWITEM 用它找回统一菜单行
        if (rows_[i].separator) {
            item.fType = MFT_SEPARATOR;
        } else {
            item.fMask |= MIIM_STRING;
            item.fType = MFT_OWNERDRAW;
            // 保留助记符串给系统做键盘定位；自绘用 drawRows_ 的无 & 显示副本。
            item.dwTypeData = const_cast<wchar_t*>(rows_[i].label.c_str());
            item.cch = static_cast<UINT>(rows_[i].label.size());
        }
        if (!InsertMenuItemW(hmenu, indexMenu + i, TRUE, &item))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, count);
}

HRESULT STDMETHODCALLTYPE ShellContextMenu::InvokeCommand(LPCMINVOKECOMMANDINFO info) {
    if (!info || !invoke_) return E_INVALIDARG;
    if (HIWORD(info->lpVerb) != 0) return E_INVALIDARG;  // 不暴露字符串动词
    const size_t i = static_cast<size_t>(LOWORD(info->lpVerb));
    if (i >= rows_.size() || rows_[i].separator || rows_[i].disabled) return E_INVALIDARG;
    return invoke_(rows_[i].data) ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE ShellContextMenu::GetCommandString(UINT_PTR, UINT, UINT*, LPSTR,
                                                            UINT) {
    // 菜单项没有对外动词/帮助串；不写输出参数，交回 shell 的默认处理。
    return E_NOTIMPL;
}

bool ShellContextMenu::OnMeasureItem(MEASUREITEMSTRUCT* item) const {
    if (!item || item->CtlType != ODT_MENU || !theme_) return false;
    const size_t i = static_cast<size_t>(item->itemData);
    if (i >= drawRows_.size()) return false;

    // 与 ShowPopupList 同一把尺子：行高 S(24)、分隔线 S(2)，
    // 宽度 = 文案 + 图标槽 + 8dp + 快捷键列 + 边距 + 最小宽。
    // 不再用 SM_CYMENU / lfMenuFont / SM_CXSMICON 拼第二套系统菜单口径。
    item->itemWidth = static_cast<UINT>(
        PopupMeasureWidth(nullptr, *theme_, &drawRows_, nullptr, theme_->iconSmall(),
                          false, 0));
    const int h = drawRows_[i].separator ? theme_->S(2) : theme_->S(24);
    item->itemHeight = static_cast<UINT>(std::max(1, h));
    return true;
}

bool ShellContextMenu::OnDrawItem(DRAWITEMSTRUCT* item) const {
    if (!item || item->CtlType != ODT_MENU || !theme_) return false;
    const size_t i = static_cast<size_t>(item->itemData);
    if (i >= drawRows_.size()) return false;

    // 原生菜单的 itemID 是 wID（100+i），PopupDrawItem 把它当行号索引。
    // 这里按 itemData 重映射一份快照，让绘制器看到的行号与 ui::Menu 完全一致。
    DRAWITEMSTRUCT rowDraw = *item;
    rowDraw.itemID = static_cast<UINT>(i);

    PopupDrawParams dp;
    dp.rows = &drawRows_;
    dp.theme = theme_;
    dp.iconS = theme_->iconSmall();
    // ODS_SELECTED 用 brListSel，ODS_HOTLIGHT 用 brListHot，其余 brList ——
    // 这就是书签菜单/所有自绘弹层的同一套高亮口径。
    dp.hot = (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) ? static_cast<int>(i) : -1;
    dp.curSel = -1;
    dp.checkCol = HasCheckedRow(drawRows_);
    return PopupDrawItem(item->hDC, &rowDraw, dp);
}

HRESULT STDMETHODCALLTYPE ShellContextMenu::HandleMenuMsg(UINT msg, WPARAM, LPARAM lp) {
    return HandleMenuMsg2(msg, 0, lp, nullptr);
}

HRESULT STDMETHODCALLTYPE ShellContextMenu::HandleMenuMsg2(UINT msg, WPARAM, LPARAM lp,
                                                          LRESULT* result) {
    if (msg == WM_MEASUREITEM && OnMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) {
        if (result) *result = TRUE;
        return S_OK;
    }
    if (msg == WM_DRAWITEM && OnDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) {
        if (result) *result = TRUE;
        return S_OK;
    }
    return S_FALSE;
}

}  // namespace ui
