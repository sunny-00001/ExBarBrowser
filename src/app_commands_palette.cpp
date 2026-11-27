// 弹层与命令面板：把"打开一个浮层"这件事的各条实现放一处。
//
// 【为什么从 app_commands.cpp 拆出来】那个文件是**命令注册的调度表**
// （各域的 app_commands_*.cpp 由它串起来），而这里是**界面侧的弹层**：
// 标签切换器 / 标签菜单 / 状态栏菜单 / 命令面板。
// 两种语义混在一个文件里，读的人要翻到文件末尾才发现"哦，这里还有一套
// 弹层实现"—— 而弹层恰恰是"人工可达路径"那一半，规则变化最频繁。
//
// 【一个不许丢的细节】破坏性命令的确认门收在 CommandBus::Dispatch
// （所有人工入口统一一道门）；本文件里的 InvokeFromPalette 只保留
// **日常白名单**上那部分确认框。动这段时别把它当成"重复确认"删掉 ——
// 它是命令面板独有的宽松处（命令面板历来对每条破坏性命令都问一遍）。
#include "app.h"

#include "app_commands.h"
#include "commands.h"
#include "config.h"
#include "icon_glyph.h"
#include "menu.h"
#include "prompt.h"
#include "status_bar.h"
#include "status_segs.h"
#include "str.h"
#include "sys_icon.h"
#include "tab_set.h"

#include <algorithm>
#include <string>

// ---------------------------------------------------------------- 菜单
// 标签切换器：与"溢出 » 菜单"同源的全局入口（Ctrl+Shift+A），预选当前标签、
// 支持键入前缀定位（PopupList 的 type-ahead）；同一份浮层，行为才不与菜单分叉。
namespace {
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
        // 必须自己判范围：json::Value::MakeInt 收 int，超出 int 范围直接
        // static_cast 是**实现定义**的截断 —— "9999999999" 会变成负数喂给
        // config.set（比如把超时设成负值）。宁可拒绝，也不静默改成一个假值。
        if (v < INT_MIN || v > INT_MAX) return false;
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
    // 选中某一行 = tab.activate：与快捷键/AI 同源（前置检查、审计、断言只有一处）。
    // 以前这里直调 Activate —— 弹层走命令、选中直调，同一弹窗两段两条强度。
    json::Value a = json::Value::MakeObject();
    a.Set(L"index", json::Value::MakeInt(action));
    (void)CommandBus::Get().Invoke(CmdId::TabActivate, Args::FromJson(a), Actor::Ui());
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

    // 关闭历史条目：与 tab.reopen 同一命令，序号决定重开第几条（命令 side 从
    // 关闭栈按序取，这里把序号转成 index 参数传给 handler 的历史序号口径）。
    // 先于 switch 处理，避免为 20 条历史各写一个 case。
    //
    // 【必须排在 ref 检查之前】重开不依赖任何存活标签 —— 菜单里的"最近关闭"
    // 那一段，恰恰是在标签集合可能已变（菜单是模态的，期间 AI 命令能关标签）
    // 时最该可用的动作。放在 `if (!ref) return;` 后面会让它被静默吞掉。
    if (action >= kTabMenuReopenBase) {
        json::Value a = json::Value::MakeObject();
        a.Set(L"index", json::Value::MakeInt(action - kTabMenuReopenBase));
        CommandBus::Get().Invoke(CmdId::TabReopenAt, Args::FromJson(a), Actor::Ui());
        return;
    }

    const int id = tabSet_.IdAt(index);
    const core::TabRef* ref = tabSet_.Find(id);
    if (!ref) return;   // 下面每个动作都要作用在某个标签上；找不到就放弃
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
    // D7：段下标 0..Seg_Count-1 与菜单动作号必须显式分段。以前靠
    // "100/101 不会和段下标撞"这条隐式约定，加一项就可能静默错位。
    enum StatusAction {
        kCopyPath = 100,
        kHideStatusBar = 101,
    };
    ui::Menu menu;
    const auto& enabled = Config::Get().s().statusSegs;
    for (int i = 0; i < Seg_Count; ++i) {
        // 常驻段不进勾选列表：SetEnabledSegments 会强制补上，勾掉也白勾。
        if (StatusSegAlwaysOn(i)) continue;
        const bool on = std::find(enabled.begin(), enabled.end(), i) != enabled.end();
        // 段名走 StatusSegmentName（与图标表同处，见 status_bar.cpp）：以前这里
        // 有一张**漏项**的本地静态表（少 Seg_Ai 一项 = nullptr），而 AddSystemIcon
        // 收的是 const std::wstring& —— 从 nullptr 构造直接崩（菜单一点就崩）。
        // 勾选前缀由组件统一拼；段图标与状态栏本体共用同一份映射。
        menu.AddSystemIcon(StatusSegmentName(i), on, i, sysicon::Get(StatusSegmentSysId(i)),
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
