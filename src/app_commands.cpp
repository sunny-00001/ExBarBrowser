// 命令注册入口 + 通用动作 + 菜单。
//
// 注册按域分片（N2）：标签/跳转/导航/路径 -> app_commands_tabs，文件操作/视图 ->
// app_commands_files，窗口系统/外部/书签/会话 -> app_commands_misc。本文件只做调度。
#include "app.h"

#include "app_commands.h"
#include "view_modes.h"
#include "clipboard.h"
#include "shell_ops.h"
#include "hotkey_manager.h"
#include "menu.h"
#include "prompt.h"

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
    Tab* t = Active();
    return t && t->view ? t->view->CurrentView() : nullptr;
}

void App::CopyText(const std::wstring& text) {
    // 剪贴板写入唯一实现（clipboard.cpp）—— 以前这里和 BookmarkBar 各写一遍。
    if (!SetClipboardText(hwnd_, text)) LogDbg(L"[clip] 写入失败（剪贴板被占用？）");
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
    ShellExecuteW(hwnd_, L"open", ExePath().c_str(), (L"\"" + path + L"\"").c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void App::ApplyViewMode(int index) {
    if (index < 0 || index >= static_cast<int>(kViewModeCount)) return;
    Tab* t = Active();
    if (!t || !t->view) return;
    t->view->SetViewMode(kViewModes[index].fvm, kViewModes[index].icon);
    viewMode_ = index;
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
    const std::vector<std::wstring> targets = args.StrArray(L"targets");
    const std::wstring to = args.Str(L"to");
    if (targets.empty() || to.empty()) return false;

    return WithView([&](IShellView* v) {
        // 与 FsDelete 同样的一次性做法：全部选中 -> 一次转移。
        // 循环里"选一个转一个"会产生 N 次 IFileOperation 与 N 次刷新，
        // 而且中途失败时用户看到的是一半已移动、一半没动。
        const int hit = shellops::SelectByPaths(v, targets);
        if (hit == 0) return false;
        const bool ok = shellops::TransferToFolder(v, hwnd_, to, move);
        RefreshActive();
        if (hit < static_cast<int>(targets.size()))
            LogDbg(L"[cmd] %ls 只命中 %d/%zu 个目标", move ? L"FsMoveTo" : L"FsCopyTo", hit,
                   targets.size());
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
    RegisterMiscCommands();
}

// ---------------------------------------------------------------- 菜单
void App::ShowTabMenu(int index, int action) {
    int id = tabSet_.IdAt(index);
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    switch (action) {
    // "新建标签页"与"+"按钮、tab.new 同义：开一个此电脑作为全新起点。
    // 以前它和 kTabMenuClone 一模一样（都复制当前路径），两个菜单项名不副实 ——
    // 现在"新建"= 新起点、"复制标签"= 同路径副本，语义分开。
    case kTabMenuNew: NewTab(nav::kThisPc, NewTabIndex()); break;
    case kTabMenuClose: CloseTab(id); break;
    case kTabMenuCloseOther: CloseOthers(id); break;
    case kTabMenuCloseLeft: CloseSide(id, true); break;
    case kTabMenuCloseRight: CloseSide(id, false); break;
    case kTabMenuClone: NewTab(ref->path, NewTabIndex()); break;
    case kTabMenuToggleLock: ToggleLock(id); break;
    case kTabMenuCopyPath: CopyText(ref->path); break;
    case kTabMenuCopyTitle: CopyText(core::DisplayTitle(*ref)); break;
    case kTabMenuRename: {
        std::wstring name = core::DisplayTitle(*ref);
        // 输入框初值给"当前显示名"，用户直接改比较顺手；清空即取消改名。
        if (PromptText(hwnd_, theme_, L"重命名标签页", L"标签名称", name, &name))
            RenameTab(id, name);
        break;
    }
    case kTabMenuDedup: {
        const int closed = CloseDuplicates(id);
        if (!closed) MessageBeep(MB_ICONASTERISK);  // 全被锁定挡住了，别静默
        break;
    }
    case kTabMenuMoveLeft: MoveTab(id, tabSet_.IndexOf(id) - 1); break;
    case kTabMenuMoveRight: MoveTab(id, tabSet_.IndexOf(id) + 1); break;
    case kTabMenuMoveFirst: MoveTab(id, 0); break;
    case kTabMenuMoveLast: MoveTab(id, tabSet_.Count() - 1); break;
    case kTabMenuReopen: {
        std::wstring path;
        if (tabSet_.PopClosed(&path)) NewTab(path);
        break;
    }
    default:
        break;
    }
}

// 标签颜色：从 WM_APP_TAB_COLOR 来（颜色是"值"，不是"动作"）。
// index 越界（菜单期间标签被关掉）时静默返回 —— SetColor 自己也会夹取值域。
void App::ShowTabColor(int index, int color) {
    const int id = tabSet_.IdAt(index);
    if (!id) return;
    SetTabColor(id, color);
}

void App::ShowStatusMenu(POINT screen) {
    static const wchar_t* kSegNames[Seg_Count] = {
        L"文件夹计数", L"文件计数", L"已选/总数", L"修改日期", L"路径",
        L"选中容量",   L"容器/磁盘", L"隐藏文件三态", L"类型",
    };
    ui::Menu menu;
    const auto& enabled = Config::Get().s().statusSegs;
    for (int i = 0; i < Seg_Count; ++i) {
        const bool on = std::find(enabled.begin(), enabled.end(), i) != enabled.end();
        // 勾选前缀由组件统一拼；段图标与状态栏本体共用同一份映射。
        menu.AddChecked(kSegNames[i], on, i, StatusSegmentIcon(i));
    }
    menu.Separator()
        .Add(L"复制当前路径", 100, ico::IconId::Copy)
        .Add(L"隐藏状态栏", 101, ico::IconId::Eye);

    ui::MenuOptions opt;
    opt.toggleKey = L"status.menu";
    const int data = menu.Show(hwnd_, theme_, screen, opt);
    if (data < 0) return;
    if (data == 100) {
        CommandBus::Get().Invoke(CmdId::PathCopyDir, Args::Empty(), L"ui");
        return;
    }
    if (data == 101) {
        Config::Get().mutableSettings().showStatus = false;
        Config::Get().Save();
        Layout();
        return;
    }
    if (data < 0) return;

    auto& segs = Config::Get().mutableSettings().statusSegs;
    auto it = std::find(segs.begin(), segs.end(), data);
    if (it == segs.end())
        segs.push_back(data);
    else
        segs.erase(it);
    // 状态栏右对齐依赖 layout_ 中「左段在前、右段在后」的稳定顺序；
    // push_back 会把重新开启的段追加到末尾，导致段错位并持久化到配置。
    std::sort(segs.begin(), segs.end());
    Config::Get().Save();
    statusBar_.SetEnabledSegments(segs);
    Layout();
}

void App::ShowCommandPalette() {
    std::vector<const CommandDef*> defs;
    ui::Menu menu;
    for (const auto& d : CommandTable()) {
        if (!d.implemented) continue;
        // 面板只能给空参。带必填参数的命令若混进来，handler 会收到空值：
        // tab.rename 会被当成"清空别名"、tab.color 会被当成"设成无颜色"，
        // 都是静默丢用户数据。参数命令走各自 UI（右键菜单/热键参数/AI）。
        if (d.argsSchema && std::string(d.argsSchema).find("\"required\"") != std::string::npos)
            continue;
        std::wstring label = std::wstring(d.title) + L"    [" + d.group + L"]";
        // 命令面板统一用搜索字形：它表示“在这里找命令”，不假装每条命令各有图标。
        menu.Add(label, static_cast<int>(defs.size()), ico::IconId::Search);
        defs.push_back(&d);
    }
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    POINT at{rc.left + theme_.S(80), rc.top + theme_.S(120)};
    // 命令面板是键盘触发的固定入口（不是某一项的右键），键固定即可：
    // 连按两次快捷键 → 第一次开、第二次收起，与所有下拉同一契约。
    ui::MenuOptions opt;
    opt.minWidthPx = theme_.S(360);
    opt.toggleKey = L"cmd.palette";
    const int idx = menu.Show(hwnd_, theme_, at, opt);
    if (idx < 0 || idx >= static_cast<int>(defs.size())) return;
    CommandBus::Get().Invoke(defs[static_cast<size_t>(idx)]->id, Args::Empty(), L"palette");
}

void App::ShowViewMenuAt() { navBar_.ShowViewMenuNow(); }
