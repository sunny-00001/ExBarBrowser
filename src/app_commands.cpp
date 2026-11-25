// 命令注册入口 + 通用动作 + 菜单。
//
// 注册按域分片（N2）：标签/跳转/导航/路径 -> app_commands_tabs，文件操作/视图 ->
// app_commands_files，窗口系统/外部/书签/会话 -> app_commands_misc。本文件只做调度。
#include "app.h"

#include "app_commands.h"
#include "view_modes.h"
#include "clipboard.h"
#include "file_undo.h"
#include "path_util.h"
#include "shell_ops.h"
#include "hotkey_manager.h"
#include "menu.h"
#include "prompt.h"
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

// ---- 命令面板的参数收集 ----
//
// 面板要能触发"带必填参数"的命令，所以需要：判断有没有必填参数、以及把它们
// 从用户输入里收集成一份合法参数对象。参数一律**从 schema 读**，不在这里硬编码
// 任何命令知识（命令表是唯一真源）。

// 解析 argsSchema（失败视为无 schema）。
static bool ParseArgsSchema(const CommandDef& d, json::Value* schema) {
    if (!d.argsSchema || !d.argsSchema[0]) {
        *schema = json::Value::MakeObject();
        return true;
    }
    std::wstring perr;
    if (!json::Parse(str::FromUtf8(d.argsSchema), *schema, &perr) || !schema->IsObject()) {
        return false;   // schema 非法是实现缺陷，面板不该静默当成"无参数"
    }
    return true;
}

// 是否声明了必填参数。收口到 HasRequiredArgs（命令面板与菜单键位共用同一答案）。
static bool HasRequiredParams(const CommandDef& d) { return HasRequiredArgs(d); }

// 把一段文本按参数类型解释成 JSON 值。
// 失败返回 false（面板会就此取消，绝不把一个空值塞进命令）。
static bool CoerceParam(const std::wstring& key, const std::wstring& type,
                        const std::wstring& text, json::Value* out) {
    std::wstring desc = str::Trim(text);
    if (type == L"integer" || type == L"number") {
        // 严格取整：带小数点/非数字一律拒绝，避免把 "3.7" 静默截成 3。
        std::wstring digits = desc;
        if (digits.empty()) return false;
        for (auto& c : digits) {
            if (c < L'0' || c > L'9') {
                if (c == L'-' && digits.find(L'-') == 0 && std::count(digits.begin(),
                                                                     digits.end(),
                                                                     L'-') == 1)
                    continue;
                return false;
            }
        }
        // 用 wcstoll 直接在宽字符上取值：先转 UTF-8 再 strtoll 会多一次
        // "转换失败也不报错"的机会，而这里只有纯数字串。
        errno = 0;
        wchar_t* end = nullptr;
        const long long v = std::wcstoll(digits.c_str(), &end, 10);
        if (errno != 0 || end == digits.c_str() || (end && *end != L'\0')) return false;
        *out = json::Value::MakeInt(static_cast<int>(v));
        return true;
    }
    if (type == L"boolean") {
        // 常见几种写法都认，但认不清就拒绝（不猜默认值 —— 猜错会让开关状态反了）。
        const std::wstring low = str::Lower(desc);
        if (low == L"true" || low == L"1" || low == L"yes" || low == L"是" || low == L"开") {
            *out = json::Value::MakeBool(true);
            return true;
        }
        if (low == L"false" || low == L"0" || low == L"no" || low == L"否" || low == L"关") {
            *out = json::Value::MakeBool(false);
            return true;
        }
        return false;
    }
    if (type == L"array") {
        // 分号分隔的字符串数组（适合"一组路径/一组目标"的参数）。
        json::Value arr = json::Value::MakeArray();
        size_t start = 0;
        while (start <= desc.size()) {
            const size_t semi = desc.find(L';', start);
            const std::wstring one = str::Trim(desc.substr(
                start, semi == std::wstring::npos ? std::wstring::npos : semi - start));
            if (!one.empty()) arr.arr.push_back(json::Value::MakeStr(one));
            if (semi == std::wstring::npos) break;
            start = semi + 1;
        }
        if (arr.arr.empty()) return false;
        *out = arr;
        return true;
    }
    // string 或未声明类型的参数（如 config.set 的 value）：保留原文。
    *out = json::Value::MakeStr(desc);
    return true;
}

// 交互式收集必填参数。返回 false = 用户取消或某个参数无法解析。
// err 用于给出人可读的原因（写进日志，便于排查"面板里这条命令为什么没执行"）。
// owner/theme 由调用方传进来：对话框要靠它们定位与配色，匿名命名空间里拿不到 App。
static bool CollectParamsInteractive(HWND owner, const Theme& theme, const CommandDef& d,
                                     json::Value* out, std::wstring* err) {
    json::Value schema;
    if (!ParseArgsSchema(d, &schema)) {
        if (err) *err = L"参数 schema 解析失败";
        return false;
    }
    const json::Value* req = schema.Find(L"required");
    const json::Value* props = schema.Find(L"properties");
    if (!req || !req->IsArray() || !props || !props->IsObject()) {
        if (err) *err = L"schema 缺少 required/properties";
        return false;
    }

    json::Value args = json::Value::MakeObject();
    for (const auto& rkey : req->arr) {
        if (!rkey.IsString()) continue;
        const std::wstring key = rkey.str;
        const json::Value* prop = props->Find(key.c_str());
        const std::wstring type = prop ? prop->GetStr(L"type") : L"string";
        const std::wstring desc = prop ? prop->GetStr(L"description") : key;

        // 用 desc 做输入框初值/提示，标题带上命令名，用户能分清在填哪个参数。
        std::wstring label = key;
        if (!desc.empty()) label += L" —— " + desc;
        std::wstring text;
        // 数组/未声明类型的参数走"原始文本"框；其余也走文本框再按类型解释。
        const bool raw = (type == L"array" || type.empty());
        if (!PromptText(owner, theme, std::wstring(d.title) + L" · 参数 " + key, label,
                        L"", &text)) {
            return false;   // 用户取消：什么都不做，不调用命令
        }

        json::Value val;
        if (raw && type == L"array") {
            if (!CoerceParam(key, type, text, &val)) {
                if (err) *err = L"参数 " + key + L" 需要分号分隔的列表";
                return false;
            }
        } else if (!CoerceParam(key, type, text, &val)) {
            if (err) *err = L"参数 " + key + L" 需要 " + type;
            return false;
        }
        args.Set(key, std::move(val));
    }
    *out = std::move(args);
    return true;
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
    // 档位也要跟着走：viewMode_ 是"当前档位"的唯一活状态（状态栏、视图菜单、
    // ai::State 都读它）。只改配置不改这里，表现是"视图变了、菜单还打勾在旧档"。
    // 这条赋值在新建标签时是幂等的（值来自同一份配置），所以不必分两种调用方。
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
    // 路径归一化（正斜杠→反斜杠、折叠连续分隔符）：
    // 外部给的 "C:/dir/file.txt" 在 Windows 里是合法写法，但 shell 的
    // 解析 API 只认反斜杠 —— 不归一化会得到 E_INVALIDARG，而报错完全
    // 指不出真正原因。撤销记录里存的也必须是归一化后的形式。
    std::vector<std::wstring> targets;
    targets.reserve(args.StrArray(L"targets").size());
    for (const auto& p : args.StrArray(L"targets")) targets.push_back(pathutil::ToShellPath(p));
    const std::wstring to = pathutil::ToShellPath(args.Str(L"to"));
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
        const bool ok = shellops::TransferToFolder(v, hwnd_, to, move);
        RefreshActive();
        if (hit < static_cast<int>(targets.size()))
            LogDbg(L"[cmd] %ls 只命中 %d/%zu 个目标", move ? L"FsMoveTo" : L"FsCopyTo", hit,
                   targets.size());
        // 撤销记录：源路径 → 目标路径。撤销移动 = 移回原目录；撤销复制 = 删副本。
        //
        // 落点按"目标目录 + 原文件名"算出来。冲突时 shell 会自动改名
        //（FOF_RENAMEONCOLLISION，比如 "a (2).txt"）—— 那种情况下算出来的
        // 落点并不存在，**不记**这一项：记一条指向不存在路径的撤销记录，
        // 等于给用户一个"撤销会失败"的假承诺。
        if (ok) {
            const std::vector<std::wstring> sel = shellops::SelectedPaths(v);
            std::vector<fileundo::Item> items;
            for (const std::wstring& p : sel) {
                const std::wstring leaf = nav::Leaf(p);
                if (leaf.empty()) continue;
                const std::wstring after = tf::Join(to, leaf);
                if (!tf::Exists(after)) {
                    LogDbg(L"[undo] %ls 的落点与预期不符（%s），不记撤销记录",
                           move ? L"FsMoveTo" : L"FsCopyTo", after.c_str());
                    continue;
                }
                fileundo::Item item;
                item.beforePath = p;
                item.afterPath = after;
                item.beforeName = leaf;
                items.push_back(std::move(item));
            }
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
    RegisterExternalCommands();
    RegisterBookmarkIoCommands();
    RegisterWindowCommands();
    RegisterDevCommands();
    RegisterArchiveCommands();
    RegisterSearchCommands();
    RegisterAiPanelCommands();
}

// ---------------------------------------------------------------- 菜单
// 标签切换器：与"溢出 » 菜单"同源的全局入口（Ctrl+Shift+A），预选当前标签、
// 支持键入前缀定位（PopupList 的 type-ahead）；同一份浮层，行为才不与菜单分叉。
void App::ShowTabSwitcher() {
    const int n = tabSet_.Count();
    if (n <= 0) return;
    ui::Menu menu;
    int initial = -1;
    const int active = ActiveIndex();
    for (int i = 0; i < n; ++i) {
        const auto& t = tabSet_.items()[static_cast<size_t>(i)];
        std::wstring label = core::DisplayTitle(t);
        if (label.empty()) label = L"新标签";
        const int row = menu.AddPathChecked(label, i == active, t.path, i, false);
        if (i == active) initial = row;
    }
    POINT at{};
    GetCursorPos(&at);
    ui::MenuOptions opt;
    opt.maxRows = std::max(8, std::min(n, 24));
    opt.toggleKey = L"tab.switcher";
    opt.initialRow = initial;
    const int action = menu.Show(hwnd_, theme_, at, opt);
    if (action < 0) return;                 // 取消：不改状态
    const int id = tabSet_.IdAt(action);    // action 就是标签下标
    if (id) Activate(id);
}

void App::ShowTabMenu(int index, int action) {
    // 菜单所有动作统一走 CommandBus：与快捷键/命令面板同源，于是审计、事件、
    // 破坏性确认、前置拒绝理由全都在这条路径上。以前这里直接调 App 方法，
    // 同一动作"快捷键有审计、右键没审计"，是第 16d 项抓出的假覆盖。
    // 目标一律传 index —— 右键可以落在非活动标签上，命令默认取活动标签会做错对象。
    auto invokeIndex = [index](CmdId cmd) {
        json::Value a = json::Value::MakeObject();
        a.Set(L"index", json::Value::MakeInt(index));
        return CommandBus::Get().Invoke(cmd, Args::FromJson(a), Actor::Ui());
    };

    const int id = tabSet_.IdAt(index);
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;
    // 关闭历史条目：与 tab.reopen 同一命令，序号决定重开第几条（命令 side 从
    // 关闭栈按序取，这里把序号转成 index 参数传给 handler 的历史序号口径）。
    // 先于 switch 处理，避免为 20 条历史各写一个 case。
    if (action >= kTabMenuReopenBase) {
        json::Value a = json::Value::MakeObject();
        a.Set(L"index", json::Value::MakeInt(action - kTabMenuReopenBase));
        CommandBus::Get().Invoke(CmdId::TabReopenAt, Args::FromJson(a), Actor::Ui());
        return;
    }
    switch (action) {
    // "新建标签页"与"+"按钮、tab.new 同义：开一个此电脑作为全新起点。
    // 以前它和 kTabMenuClone 一模一样（都复制当前路径），两个菜单项名不副实 ——
    // 现在"新建"= 新起点、"复制标签"= 同路径副本，语义分开。
    case kTabMenuNew:
        CommandBus::Get().Invoke(CmdId::TabNew, Args::Empty(), Actor::Ui());
        break;
    case kTabMenuSwitcher:
        CommandBus::Get().Invoke(CmdId::TabSwitcher, Args::Empty(), Actor::Ui());
        break;
    case kTabMenuClose: invokeIndex(CmdId::TabClose); break;
    case kTabMenuCloseOther: invokeIndex(CmdId::TabCloseOther); break;
    case kTabMenuCloseLeft: invokeIndex(CmdId::TabCloseLeft); break;
    case kTabMenuCloseRight: invokeIndex(CmdId::TabCloseRight); break;
    case kTabMenuClone: invokeIndex(CmdId::TabClone); break;
    case kTabMenuToggleLock: invokeIndex(CmdId::TabLock); break;
    case kTabMenuTogglePin: invokeIndex(CmdId::TabPin); break;
    case kTabMenuCopyPath: {
        // 复制路径走 clipboard.copy（带 text 参数），不是无参命令。
        json::Value a = json::Value::MakeObject();
        a.Set(L"text", json::Value::MakeStr(ref->path));
        CommandBus::Get().Invoke(CmdId::ClipboardCopy, Args::FromJson(a), Actor::Ui());
        break;
    }
    case kTabMenuCopyTitle: invokeIndex(CmdId::TabCopyTitle); break;
    case kTabMenuRename: {
        std::wstring name = core::DisplayTitle(*ref);
        // 输入框初值给"当前显示名"，用户直接改比较顺手；清空即取消改名。
        if (PromptText(hwnd_, theme_, L"重命名标签页", L"标签名称", name, &name)) {
            json::Value a = json::Value::MakeObject();
            a.Set(L"index", json::Value::MakeInt(index));
            a.Set(L"name", json::Value::MakeStr(name));
            CommandBus::Get().Invoke(CmdId::TabRename, Args::FromJson(a), Actor::Ui());
        }
        break;
    }
    case kTabMenuDedup: {
        const bool ok = invokeIndex(CmdId::TabCloseDuplicates);
        if (!ok) MessageBeep(MB_ICONASTERISK);  // 全被锁定挡住了，别静默
        break;
    }
    case kTabMenuMoveLeft: invokeIndex(CmdId::TabMoveLeft); break;
    case kTabMenuMoveRight: invokeIndex(CmdId::TabMoveRight); break;
    case kTabMenuMoveFirst: invokeIndex(CmdId::TabMoveFirst); break;
    case kTabMenuMoveLast: invokeIndex(CmdId::TabMoveLast); break;
    case kTabMenuMoveNewWindow: invokeIndex(CmdId::TabMoveNewWindow); break;
    case kTabMenuReopen:
        // 无参重开最近关闭的一条，与菜单 .Cmd(CmdId::TabReopenClosed) 标记一致。
        CommandBus::Get().Invoke(CmdId::TabReopenClosed, Args::Empty(), Actor::Ui());
        break;
    default:
        break;
    }
}

void App::ShowStatusMenu(POINT screen) {
    static const wchar_t* kSegNames[Seg_Count] = {
        L"文件夹计数", L"文件计数", L"已选/总数", L"修改日期", L"路径",
        L"选中容量",   L"容器/磁盘", L"隐藏文件三态", L"类型",
    };
    // D7：段下标 0..Seg_Count-1 与菜单动作号必须显式分段。以前靠
    // "100/101 不会和段下标撞"这条隐式约定，加一项就可能静默错位。
    enum StatusAction {
        kCopyPath = 100,
        kHideStatusBar = 101,
    };
    ui::Menu menu;
    const auto& enabled = Config::Get().s().statusSegs;
    for (int i = 0; i < Seg_Count; ++i) {
        const bool on = std::find(enabled.begin(), enabled.end(), i) != enabled.end();
        // 勾选前缀由组件统一拼；段图标与状态栏本体共用同一份映射 ——
        // 优先系统默认位图，取不到退字形（两处长得一样）。
        menu.AddSystemIcon(kSegNames[i], on, i, sysicon::Get(StatusSegmentSysId(i)),
                           StatusSegmentIcon(i));
    }
    // 快捷键由键位表自动推出（见 menu.h）。「隐藏状态栏」只是配置键，无对应命令。
    menu.Separator()
        .Add(L"复制当前路径", kCopyPath, ico::IconId::Copy).Cmd(CmdId::PathCopyDir)
        .Add(L"隐藏状态栏", kHideStatusBar, ico::IconId::Eye);

    ui::MenuOptions opt;
    opt.toggleKey = L"status.menu";
    const int data = menu.Show(hwnd_, theme_, screen, opt);
    if (data == kCopyPath) {
        CommandBus::Get().Invoke(CmdId::PathCopyDir, Args::Empty(), Actor::Ui());
        return;
    }
    if (data == kHideStatusBar) {
        // 走 config.set 而不是直接改 Settings：键表校验 + 写盘 + 状态栏重建
        // + 审计都收在一条路径。过去这里直接 mutableSettings()+Save()+Layout()，
        // 机器侧既看不见也调不到（FULL-COVERAGE §10.1 的一个写路径）。
        json::Value a = json::Value::MakeObject();
        a.Set(L"key", json::Value::MakeStr(L"statusbar"));
        a.Set(L"value", json::Value::MakeBool(false));
        CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(a), Actor::Ui());
        return;
    }
    if (data < 0 || data >= Seg_Count) return;  // 只剩段下标；越界一律不当段处理

    // 段勾选同样转投 config.set statusSegs：值由这里算好（整段数组），
    // 命令层只负责校验/落盘/重建。过去这段直接 push_back/erase + Save，
    // 是"人工可达但无结构化入口"的又一处。
    auto segs = Config::Get().s().statusSegs;
    auto it = std::find(segs.begin(), segs.end(), data);
    if (it == segs.end())
        segs.push_back(data);
    else
        segs.erase(it);
    // 状态栏右对齐依赖 layout_ 中「左段在前、右段在后」的稳定顺序；
    // push_back 会把重新开启的段追加到末尾，导致段错位并持久化到配置。
    std::sort(segs.begin(), segs.end());
    json::Value args = json::Value::MakeObject();
    args.Set(L"key", json::Value::MakeStr(L"statusSegs"));
    json::Value arr = json::Value::MakeArray();
    for (int n : segs) arr.arr.push_back(json::Value::MakeInt(n));
    args.Set(L"value", std::move(arr));
    CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(args), Actor::Ui());
}

void App::InvokeFromPalette(const CommandDef& d) {
    // 破坏性命令的确认**收口在 CommandBus::Dispatch**（所有人工入口统一一道门：
    // 命令面板 / 序列 / 弹层菜单 / 快捷键 / 鼠标）。以前这道门只写在这个函数里，
    // 于是序列与弹层菜单能绕过它直接执行 —— 实测：序列 q x 直接清空 19 条
    // 搜索历史，一道确认都没有。
    //
    // 这里只为 NeedsConfirm() 明确放行的**日常白名单**保留原确认框
    // （关标签、跑构建/测试这类高频且可恢复的动作）。命令面板历来对每条
    // 破坏性命令都问一遍，这个行为不该因为收口而变松。
    // NeedsConfirm 现在只在白名单上返回 false；undoable 本身不再是豁免。
    if (d.effects.destructive && !CommandBus::Get().NeedsConfirm(d)) {
        const int yes = MessageBoxW(hwnd_,
            (L"「" + std::wstring(d.title) + L"」会覆盖现有内容。确定执行吗？").c_str(),
            L"确认执行", MB_YESNO | MB_ICONWARNING);
        if (yes != IDYES) return;
    }
    // 无必填参数：直接以空参调用。
    if (!HasRequiredParams(d)) {
        CommandBus::Get().Invoke(d.id, Args::Empty(), Actor::Palette());
        return;
    }
    // 有必填参数：先追问，再调用。**绝不空参调用** —— 空参会让 tab.rename 变成
    // "清空别名"、tab.color 变成"设成无颜色"，都是静默丢用户数据。
    json::Value args = json::Value::MakeObject();
    std::wstring schemaErr;
    if (!CollectParamsInteractive(hwnd_, theme_, d, &args, &schemaErr)) {
        if (!schemaErr.empty()) LogDbg(L"[palette] %s 的参数收集失败：%s", d.name, schemaErr.c_str());
        return;   // 用户取消/参数被拒：不调用，什么也不做
    }
    CommandBus::Get().Invoke(d.id, Args::FromJson(args), Actor::Palette());
}

void App::ShowCommandPalette() {
    // 命令面板 = 序列总览弹窗的**唤起态**：一张模式列表
    // （序号│名称(含前缀)│说明），回车进入某个模式；命令清单在「命令 (!)」模式里。
    //
    // 【为什么不直接列命令】原版 SeqUiUserOpenOnHookThread：Ctrl+Shift+P 唤起时
    // g_seqMode=-1、**图标不亮** —— 而"图标不在"正是"模式列表"的判据
    // （SeqShowingModeList）。原版的命令列表是 `!` 前缀进的模式（SeqListCmd），
    // 不是唤起态的默认页。复刻后：Ctrl+Shift+P → 模式列表 → 回车进「命令 (!)」
    // → 207 条命令（打字过滤 + 分组 + 触发键），路径与原版一致。
    //
    // 过去这里另起一个 ui::Menu，与序列提示弹层是两套 UI/两套交互；
    // 现在统一走同一个弹层，"序列键弹窗"与"命令面板"是同一份代码的两种入口。
    ShowSequenceOverlay(SequenceOverlay::kUserMode);
}

void App::ShowViewMenuAt() { navBar_.ShowViewMenuNow(); }
