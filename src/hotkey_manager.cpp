#include "hotkey_manager.h"

#include "platform_win.h"

#include "chord.h"
#include "commands.h"
#include "config.h"

#include <commctrl.h>

namespace {

constexpr wchar_t kClass[] = L"ExBarBrowser_Hotkeys";

struct State {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND scope = nullptr;
    HWND hint = nullptr;
    const Theme* theme = nullptr;
    InputRouter* router = nullptr;
    bool capturing = false;
    std::wstring captureCommand;
    std::vector<BindingRow> rows;
};

State* g_state = nullptr;

}  // namespace

// 类注册需要真实 WndProc，所以先声明（以前是注册 DefWindowProcW 再事后替换 —— 
// 那不是必要手段，只是跟着上一份抄的结果）。
LRESULT CALLBACK HotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l);

namespace {

void RegisterClassOnce() { win::RegisterClassOnce(kClass, HotkeyProc); }

void SetSubItem(HWND list, int row, int col, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = col;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
}

void FillList(State* st) {
    if (!st || !st->list) return;
    SendMessageW(st->list, LVM_DELETEALLITEMS, 0, 0);

    int filter = static_cast<int>(SendMessageW(st->scope, LB_GETCURSEL, 0, 0));
    std::wstring scope;
    if (filter > 0) {
        wchar_t buf[64] = L"";
        SendMessageW(st->scope, LB_GETTEXT, static_cast<WPARAM>(filter),
                     reinterpret_cast<LPARAM>(buf));
        scope = buf;
    }

    int row = 0;
    for (const auto& r : st->rows) {
        if (!scope.empty() && r.scope != scope) continue;
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.lParam = reinterpret_cast<LPARAM>(&r);
        it.pszText = const_cast<wchar_t*>(r.title.c_str());
        SendMessageW(st->list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));

        std::wstring key = r.key.empty() ? L"（未设置）" : r.key;
        if (r.isSequence) key = L"序列 " + r.key;
        SetSubItem(st->list, row, 1, r.scope);
        SetSubItem(st->list, row, 2, key);
        std::wstring status = !r.implemented ? L"未实现"
                              : r.reserved  ? L"系统占用"
                              : r.conflict  ? L"冲突"
                                            : L"可用";
        SetSubItem(st->list, row, 3, status);
        ++row;
    }
    wchar_t head[128];
    swprintf(head, 128, L"共 %d 条（筛选后 %d 条）", static_cast<int>(st->rows.size()), row);
    SetWindowTextW(st->hint, head);
}

BindingRow* SelectedRow(State* st) {
    if (!st || !st->list) return nullptr;
    int idx = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (idx < 0) return nullptr;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = idx;
    if (!SendMessageW(st->list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it))) return nullptr;
    return reinterpret_cast<BindingRow*>(it.lParam);
}

void BeginCapture(State* st) {
    BindingRow* row = SelectedRow(st);
    if (!row) {
        MessageBoxW(st->hwnd, L"请先在列表里选择一条命令。", L"快捷键管理器", MB_ICONINFORMATION);
        return;
    }
    if (row->isSequence) {
        MessageBoxW(st->hwnd, L"序列键请在 keymap.json 中编辑（序列需要多键状态机）。",
                    L"快捷键管理器", MB_ICONINFORMATION);
        return;
    }
    if (!row->implemented) {
        MessageBoxW(st->hwnd, L"该命令在当前版本未实现，改键不会生效。", L"快捷键管理器",
                    MB_ICONWARNING);
        return;
    }
    st->capturing = true;
    st->captureCommand = row->command;
    SetWindowTextW(st->hint, L"请按下新的组合键（Esc 取消）…");
    SetFocus(st->hwnd);
}

void EndCapture(State* st) {
    st->capturing = false;
    st->captureCommand.clear();
    FillList(st);
}

void RefreshRows(State* st) {
    st->rows = st->router->Rows();
    FillList(st);
}

}  // namespace

bool HotkeyManagerVisible() { return g_state && g_state->hwnd && IsWindowVisible(g_state->hwnd); }

LRESULT CALLBACK HotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    State* st = g_state;
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (st && st->theme) FillRect(dc, &rc, st->theme->brBar);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (st && st->list) {
            // 全部按客户区重排：按钮以前只在创建时按"外框高度"放一次，WM_SIZE 不管它们，
            // 结果按钮压在列表最后几行上（列表底边 504、按钮 480..510）。
            RECT rc;
            GetClientRect(h, &rc);
            const int pad = st->theme->S(12);
            const int leftW = st->theme->S(150);
            const int btnH = st->theme->S(30);
            const int btnY = rc.bottom - pad - btnH;
            const int hintY = btnY - st->theme->S(8) - st->theme->S(20);

            int bx = pad;
            for (int id : {1001, 1002, 1003, 1004}) {
                HWND b = GetDlgItem(h, id);
                if (!b) continue;
                const int w = (id == 1001 || id == 1003) ? st->theme->S(100) : st->theme->S(70);
                MoveWindow(b, bx, btnY, w, btnH, TRUE);
                bx += w + st->theme->S(8);
            }
            MoveWindow(st->hint, pad, hintY, rc.right - pad * 2, st->theme->S(20), TRUE);

            const int listH = hintY - pad;
            MoveWindow(st->scope, pad, pad, leftW, listH, TRUE);
            MoveWindow(st->list, pad + leftW + pad, pad, rc.right - leftW - pad * 3, listH, TRUE);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (!st) break;
        if (id == 1001) {  // 编辑
            BeginCapture(st);
            return 0;
        }
        if (id == 1002) {  // 清除
            BindingRow* row = SelectedRow(st);
            if (row && !row->isSequence) {
                st->router->SetKeyForCommand(row->command, L"");
                RefreshRows(st);
            }
            return 0;
        }
        if (id == 1003) {  // 恢复默认
            if (MessageBoxW(h, L"恢复所有键位到默认值？", L"快捷键管理器",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                st->router->ResetDefaults();
                RefreshRows(st);
            }
            return 0;
        }
        if (id == 1004) {  // 关闭
            DestroyWindow(h);
            return 0;
        }
        if (id == 1100 && HIWORD(w) == LBN_SELCHANGE) {  // 范围筛选
            FillList(st);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {  // Alt+字母发 WM_SYSKEYDOWN，捕获热键时必须一并拦截
        if (!st) break;
        if (st->capturing) {
            if (w == VK_ESCAPE) {
                EndCapture(st);
                return 0;
            }
            if (w == VK_CONTROL || w == VK_SHIFT || w == VK_MENU || w == VK_LWIN || w == VK_RWIN)
                return 0;
            KeyChord c = chord::FromKey(static_cast<unsigned>(w));
            if (chord::IsReserved(c)) {
                MessageBeep(MB_ICONWARNING);
                SetWindowTextW(st->hint, L"该组合被系统占用，请换一个（Esc 取消）…");
                return 0;
            }
            st->router->SetKeyForCommand(st->captureCommand, chord::Format(c));
            RefreshRows(st);
            EndCapture(st);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (st) {
            if (st->theme) EnableWindow(GetParent(h), TRUE);
            st->hwnd = nullptr;
            delete st;
            g_state = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void ShowHotkeyManager(HWND owner, const Theme& theme, InputRouter& router) {
    if (g_state && g_state->hwnd) {
        ShowWindow(g_state->hwnd, SW_SHOW);
        SetForegroundWindow(g_state->hwnd);
        return;
    }
    RegisterClassOnce();

    auto* st = new State();
    st->theme = &theme;
    st->router = &router;
    g_state = st;

    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    int w = theme.S(820), h = theme.S(560);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    st->hwnd = CreateWindowExW(0, kClass, L"快捷键管理器", WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y,
                               w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!st->hwnd) {
        delete st;
        g_state = nullptr;
        return;
    }
    ApplyDarkTitleBar(st->hwnd, theme.dark);

    int pad = theme.S(12);
    st->scope = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, pad, pad,
                                theme.S(150), theme.S(400), st->hwnd,
                                reinterpret_cast<HMENU>(1100), GetModuleHandleW(nullptr), nullptr);
    st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               pad * 2 + theme.S(150), pad, theme.S(620), theme.S(400), st->hwnd,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    st->hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, pad, theme.S(480),
                               theme.S(700), theme.S(20), st->hwnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);

    for (HWND c : {st->scope, st->list, st->hint})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);

    SendMessageW(st->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    struct Col {
        const wchar_t* name;
        int w;
    } cols[] = {{L"命令", 280}, {L"范围", 100}, {L"键位", 180}, {L"状态", 110}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(cols[i].name);
        c.cx = theme.S(cols[i].w);
        c.iSubItem = i;
        SendMessageW(st->list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(&c));
    }

    const wchar_t* scopes[] = {L"全部", L"标签栏", L"导航栏", L"书签栏", L"文件视图",
                               L"文件操作", L"系统对话框", L"窗口", L"全局"};
    for (const wchar_t* s : scopes) SendMessageW(st->scope, LB_ADDSTRING, 0,
                                                reinterpret_cast<LPARAM>(s));
    SendMessageW(st->scope, LB_SETCURSEL, 0, 0);

    struct Btn {
        const wchar_t* text;
        int id;
        int w;
    } btns[] = {{L"编辑键位…", 1001, 100}, {L"清除", 1002, 70}, {L"恢复默认", 1003, 100},
                {L"关闭", 1004, 70}};
    int bx = pad;
    int by = h - theme.S(80);
    for (const auto& b : btns) {
        HWND btn = CreateWindowExW(0, L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx,
                                   by, theme.S(b.w), theme.S(30), st->hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.id)),
                                   GetModuleHandleW(nullptr), nullptr);
        SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(theme.font), TRUE);
        bx += theme.S(b.w) + theme.S(8);
    }

    SetThis(st->hwnd, st);
    st->rows = router.Rows();
    FillList(st);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(st->hwnd, SW_SHOW);
    SendMessageW(st->hwnd, WM_SIZE, 0, 0);
}
