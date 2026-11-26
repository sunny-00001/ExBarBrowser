// 命令注册入口 + 通用动作 + 菜单。
//
// 注册按域分片（N2）：各域的实现见对应的 app_commands_*.cpp。本文件只做调度。
#include "app.h"

#include "app_commands.h"
#include "transfer_undo.h"
#include "view_modes.h"
#include "clipboard.h"
#include "file_undo.h"
#include "path_util.h"
#include "shell_ops.h"
#include "hotkey_manager.h"
#include "text_file.h"

#include <string>

namespace {

// 视图档位表在 view_modes.h（唯一来源）——它的下标是 view.mode 命令的 arg 契约，
// 不能在这里再写第二份。别名让本文件其余代码不必改动。
inline constexpr const viewmode::ViewMode* kViewModes = viewmode::kModes;
constexpr int kViewModeCount = viewmode::kCount;

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(_countof(buf)));
    return std::wstring(buf, n);
}

}  // namespace

IShellView* App::ActiveView() {
    ShellView* host = ActiveShellViewHost();
    return host ? host->CurrentView() : nullptr;
}

bool App::CopyText(const std::wstring& text) {
    // 剪贴板写入唯一实现（clipboard.cpp）—— 以前这里和 BookmarkBar 各写一遍。
    // 失败必须回传：调用方（命令 handler）要据此报 ok=false，
    // 否则"剪贴板被占用、其实没复制"会被记成成功（审计与 AI 都会被误导）。
    if (!SetClipboardText(hwnd_, text)) {
        LogDbg(L"[clip] 写入失败（剪贴板被占用？）");
        CommandBus::Get().NoteFailure(L"剪贴板写入失败（可能被其他程序占用）",
                                      FailKind::LockConflict);
        return false;
    }
    return true;
}

void App::ShowPathProperties(const std::wstring& path) {
    if (path.empty()) return;
    // 走 shell 的"属性"动词：目录/文件/命名空间统一处理，自己不要去拼
    // SHObjectProperties 的参数形式（文件系统与虚拟项的参数不同）。
    // SEE_MASK_INVOKEIDLIST 让动词由 shell 解析 —— 与右键菜单里的"属性"完全同一份实现。
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        LogDbg(L"[props] 属性框打开失败：%s err=%lu", path.c_str(), GetLastError());
}

std::vector<std::wstring> App::SelectedOrFolder() {
    std::vector<std::wstring> out;
    IShellView* view = ActiveView();
    if (!view) return out;
    out = SelectedPaths(view, 256);
    if (out.empty()) {
        std::wstring folder = ViewFolderPath(view);
        if (!folder.empty()) out.push_back(folder);
    }
    view->Release();
    return out;
}

void App::OpenPathInNewWindow(const std::wstring& path) {
    if (path.empty()) return;
    // 必须带 --new-window：新进程默认会"转发给已有窗口并激活"，不开新窗口。
    // 这条命令的语义就是"再开一个窗口"，所以显式要求新实例（共享同一份用户数据）。
    const std::wstring args = std::wstring(id::kNewWindowArg) + L" \"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", ExePath().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

void App::ApplyViewMode(int index) {
    if (index < 0 || index >= static_cast<int>(kViewModeCount)) return;
    ShellView* host = ActiveShellViewHost();
    if (!host) return;
    host->SetViewMode(kViewModes[index].fvm, kViewModes[index].icon);
    viewMode_ = index;
    SyncZoomSlider();   // 换模式会换图标尺寸，滑块要跟着走
    // 视图模式是用户偏好（不是某个目录的状态），改了就落盘，下次启动直接套用。
    Settings& s = Config::Get().mutableSettings();
    if (s.viewMode != index) {
        s.viewMode = index;
        Config::Get().Save();
    }
}

// 把配置里的视图偏好（模式/排序/分组/窗格）套到某个标签上。
// 放在本文件是因为 kViewModes 与 shellops 只在这里同时可见；
// app_tabs.cpp 的 NewTab 只负责调用。
void App::ApplyPersistedViewState(ShellView* view) {
    if (!view) return;
    const Settings& s = Config::Get().s();
    const int mode = (s.viewMode >= 0 && s.viewMode < ViewModeCount()) ? s.viewMode : 3;
    view->SetViewMode(kViewModes[mode].fvm, kViewModes[mode].icon);
    // 档位也要跟着走：viewMode_ 是"当前档位"的唯一活状态（状态栏/视图菜单/ai::State 都读它）。
    viewMode_ = mode;

    if (IShellView* sv = view->CurrentView()) {
        // 显式传方向：启动恢复不能走"再点一次反向"的交互语义，否则降序会被翻回升序。
        shellops::SetSort(sv, s.sortMode, s.sortAscending ? 1 : 0);
        // 分组必须无条件应用：groupBy == 0 表示"取消分组"，
        // 只在 >0 时调用会让上次的分组状态残留到本次启动。
        shellops::SetGroup(sv, s.groupBy);
        sv->Release();
    }
    // 窗格状态不在这里设：ShellView::Create() 已在 InitBrowser() 之前按配置初始化，
    // 这里再调一次只是重复赋值 + 多排一个校正定时器。
}

bool App::WithView(const std::function<bool(IShellView*)>& fn, const wchar_t* what) {
    IShellView* view = ActiveView();
    if (!view) return false;
    const bool ok = fn(view);
    view->Release();
    if (!ok) {
        LogDbg(L"[cmd] %s 失败", what);
        MessageBeep(MB_ICONWARNING);
    }
    return ok;
}

bool App::TransferTargets(const Args& args, bool move) {
    // 路径归一化（正斜杠→反斜杠、折叠连续分隔符）：shell 的解析 API 只认反斜杠，
    // 不归一化会得到 E_INVALIDARG，而报错完全指不出真正原因。
    std::vector<std::wstring> targets;
    targets.reserve(args.StrArray(L"targets").size());
    for (const auto& p : args.StrArray(L"targets")) targets.push_back(pathutil::ToShellPath(p));
    const std::wstring to = pathutil::ToShellPath(args.Str(L"to"));
    if (targets.empty() || to.empty()) return false;

    return TransferSelTo(targets, to, move);
}

// 对侧即目标（fs.copy.pane / fs.move.pane 的共用体）：源=活动窗格选中项，
// 目标=另一窗格当前目录。与 TransferTargets 汇合在 TransferSelTo ——
// "选中→转移→撤销记录→刷新"只有这一份（P0-2 的根本要求：不复制第三份链路）。
bool App::TransferToOtherPane(bool move) {
    IShellView* view = ActiveView();
    if (!view) return false;
    // 选中集要在转移**之前**取：搬走之后原路径已不存在（与 transferTo 同一顺序）。
    const std::vector<std::wstring> sel = shellops::SelectedPaths(view);
    view->Release();
    if (sel.empty()) {
        CommandBus::Get().NoteFailure(L"活动窗格没有选中项");
        return false;
    }

    // 目标 = 另一窗格当前目录。单栏时先开双栏（与 fs.open.pane 同语义）：
    // "对侧即目标"的前置就是对侧存在，一按就出现对侧并转移过去。
    Tab* t = Active();
    if (!t) return false;
    if (!Config::Get().s().dualPane) SetDualPane(true);
    t = Active();   // SetDualPane 可能改标签/状态，重取
    if (!t) return false;
    const int other = t->activePane == 0 ? 1 : 0;
    Pane* p = PaneAt(t, other);
    if (!p) return false;
    // 第二窗格可能还没有视图（按需创建）：路径兜底逻辑与 EnsurePaneView 相同 ——
    // 没路径就继承主窗格/主页。这里不建视图：转移目标是目录路径，与视图无关；
    // 转移完成后用户切过去自然能看到（EnsurePaneView 在那时补建）。
    std::wstring to = p->path();
    if (to.empty()) {
        Pane* main = PaneAt(t, 0);
        to = main && !main->path().empty() ? main->path() : HomePath();
    }
    return TransferSelTo(sel, to, move);
}

// 「选中 → 转移 → 撤销记录 → 刷新」的唯一链路。
// 调用方：TransferTargets（fs.copy/fs.move，显式点名目标）与
// TransferToOtherPane（fs.copy.pane/fs.move.pane，目标=对侧）。目标来源不同，
// 其余步骤必须完全一致 —— 否则又是"人机两条路径两套行为"（P0-11 同源问题的翻版）。
bool App::TransferSelTo(const std::vector<std::wstring>& targets, const std::wstring& to,
                        bool move) {
    if (targets.empty() || to.empty()) return false;

    return WithView([&](IShellView* v) {
        // 与 FsDelete 同样的一次性做法：全部选中 -> 一次转移。
        // 循环里"选一个转一个"会产生 N 次 IFileOperation 与 N 次刷新，
        // 而且中途失败时用户看到的是一半已移动、一半没动。
        const int hit = shellops::SelectByPaths(v, targets);
        if (hit == 0) {
            // fs.* 的语义边界：只操作**当前视图里**的项（不靠路径凭空动视图外的目录）。
            // 目标不在视图里时过去只说"命令执行失败" —— 与"权限不足/被占用"混在一起，
            // 调用方没法判断该换个目录还是该换条路径（2026-11-21 实测：把 sub\a.txt
            // 移回父目录，因为 sub 不在视图里而失败）。
            CommandBus::Get().NoteFailure(L"目标不在当前视图：" + targets.front());
            return false;
        }
        // 落点预探必须在**转移之前**：转移后才探到的"已存在"分不清是
        // "我们刚产生的副本"还是"用户原本就有的同名文件"（见 transfer_undo.h）。
        const std::vector<bool> preExisted = transferundo::PrecheckDest(targets, to);
        const bool ok = shellops::TransferToFolder(v, hwnd_, to, move);
        RefreshActive();
        if (hit < static_cast<int>(targets.size()))
            LogDbg(L"[cmd] %ls 只命中 %d/%zu 个目标", move ? L"FsMoveTo" : L"FsCopyTo", hit,
                   targets.size());
        // 撤销记录：源路径 → 真实落点。
        //
        // **源用入参 targets，不用转移之后的 SelectedPaths**：移动把源项搬走之后，
        // 视图里已经没有它们了，事后读选中集拿到的是空/别的项 —— 那会让"撤销移动"
        // 整条记录成空。
        //
        // 冲突（目标目录原本就有同名文件）由 transfer_undo 过滤掉：那种情况下
        // shell 按 FOF_RENAMEONCOLLISION 改了名，真实落点不可知；照旧记一条
        // 指向用户原有文件的记录，撤销时就会删错文件。
        if (ok) {
            std::vector<fileundo::Item> items =
                transferundo::BuildItems(targets, to, preExisted);
            if (!items.empty())
                NoteFileOp(move ? fileundo::Op::Move : fileundo::Op::Copy, std::move(items),
                           move ? L"移动到" : L"复制到");
        }
        return ok;
    }, move ? L"移动到" : L"复制到");
}

int App::ViewModeCount() { return static_cast<int>(kViewModeCount); }

int App::ViewModeIndexFor(int fvm, int iconPx) {
    const int n = ViewModeCount();
    // 先精确匹配 (fvm, icon)：表里 FVM_THUMBNAIL 有 256/96/48 三档，只按 fvm 会认错。
    for (int i = 0; i < n; ++i)
        if (kViewModes[i].fvm == fvm && kViewModes[i].icon == iconPx) return i;
    // 退一步只按 fvm 匹配（图标尺寸可能被 shell 归一化过）。
    for (int i = 0; i < n; ++i)
        if (kViewModes[i].fvm == fvm) return i;
    return -1;
}

void App::RegisterCommands() {
    RegisterTabCommands();
    RegisterFileCommands();
    RegisterViewPaneCommands();
    RegisterScanCommands();
    RegisterCompareCommands();
    RegisterJobCommands();
    RegisterMiscCommands();
    RegisterConfigCommands();
    RegisterContextCommands();
    RegisterFocusCommands();
    RegisterUndoCommands();
    RegisterExternalCommands();
    RegisterBookmarkIoCommands();
    RegisterWindowCommands();
    RegisterDevCommands();
    RegisterArchiveCommands();
    RegisterSearchCommands();
    RegisterAiPanelCommands();
}
