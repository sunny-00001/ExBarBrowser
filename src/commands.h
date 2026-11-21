// 命令目录与派发（唯一动作入口）
//
// 目的：键盘 / 序列 / 鼠标 / 菜单 / 命令面板全部经同一张表与同一条派发路径，
// 保证"改键位不改代码"、"人机同源"（同一命令既能被快捷键触发，也能被面板触发）。
//
// 命名与 ExBar mod 的 data/capabilities.json 对齐；本工程未实现的命令仍然保留在表里
// （implemented=false），这样 mod 的 keymap.json 可以原样读取，未实现项在管理器里
// 明确标灰，而不是"键位静默消失"。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "json.h"

enum class CmdId : int {
    None = 0,
    // Tab
    TabNew, TabClose, TabCloseOther, TabCloseLeft, TabCloseRight, TabClone,
    TabReopenClosed, TabMoveLeft, TabMoveRight, TabMoveFirst, TabMoveLast,
    TabPrevTab, TabLast, CloseLeftOne, CloseRightOne,
    TabActivate,  // arg = 0-based index
    GoTab1, GoTab2, GoTab3, GoTab4, GoTab5, GoTab6, GoTab7, GoTab8, GoTab9,
    TabNext, TabPrev, TabMergeAll,
    // Go
    GoHome, GoDesktop, GoDownloads, GoDocuments, GoConfigPath, GoLocalAppData,
    GoControlPanel, GoStartFolder, GoThisPC, GoPowerShell,
    // Nav
    NavBack, NavForward, NavUp, NavRefresh, NavHome, NavViewMenu,
    // Path
    PathCopy, PathCopyName, PathCopyDir, PathCopyUnc, PathCopyExt, PathCopyTab, PathCopyLink,
    // File
    CmdNewTextFile, CmdOpenWith, CmdNewFolder, CmdCopy, CmdCut, CmdPaste, CmdDelete,
    CmdRename, CmdProps, CmdUndo, CmdSelectAll, CmdOpen, CmdOpenInNewTab,
    CmdOpenInNewWindow, SelectInvert, SelectFiles, SelectFolders, SelectByType,
    CopyToFolder, MoveToFolder, BatchRename, SelectByPattern,
    FileFocusTop, FileFocusBottom, CmdRefresh,
    // ---- 确定式文件操作（P5：交互与确定分离）----
    // 上面那批 Cmd* 是"交互式"：要么弹输入框/对话框，要么作用于"用户碰巧选中"的项。
    // 下面这批是同一个业务的**确定式**版本：目标由参数点名，绝不弹框、绝不读选中。
    // 两者共用 shell_ops 里的同一份实现（唯一实现原则），只是入口不同。
    FsRename,    // {target, newName}  —— 对应 CmdRename 的确定式版本
    FsDelete,    // {targets[]}        —— 对应 CmdDelete
    FsCopyTo,    // {targets[], to}    —— 对应 CopyToFolder
    FsMoveTo,    // {targets[], to}    —— 对应 MoveToFolder
    FsOpen,      // {path}             —— 对应 CmdOpen
    // FsOpen 的语义与 CmdOpen 完全一致：文件夹在当前标签内进入，文件交给 shell。
    // 不设 mode 参数是刻意的 —— VS Code 把"打开"拆成 open / openWith / revealInExplorer
    // 三条独立命令，而不是一条带 mode 的命令。带 mode 会让 schema 变成枚举，
    // 调用方要先查"有哪些 mode"，而它们其实是三种不同的意图，值得各占一个名字。
    FsOpenInNewTab,     // {path}         —— 对应 CmdOpenInNewTab
    FsOpenInNewWindow,  // {path}         —— 对应 CmdOpenInNewWindow
    FsCopy,             // {targets[]}    —— 对应 CmdCopy（写剪贴板，不产生副本）
    FsCut,              // {targets[]}    —— 对应 CmdCut（写剪贴板 + 标记移动）
    FsProperties,       // {target}       —— 对应 CmdProps
    // View
    ViewMode,  // arg = 0..7（超大/大/中/小/列表/详细信息/平铺/内容）
    ViewSort,  // arg = 0..3（名称/日期/类型/大小）
    ViewGroup, // arg = 0..3（无/类型/日期/大小）
    ToggleHidden, ToggleSystemFiles, ToggleExtensions, CycleHiddenSeg,
    TogglePreviewPane, ToggleNavPane, ToggleCheckboxes, ViewNext, ViewPrev,
    // Window
    // 注：QuitExplorer 是 mod 期的名字（那时行为是"重启 explorer.exe"）。本工程里
    // 它的行为是"退出本程序"，点分名 win.quit 才是准确描述。枚举名保留是因为
    // 规则 C1（CmdId 值一经分配不再改，废弃则保留占位）—— 改枚举名会打断
    // keymap.json / 审计日志 / 脚本里的引用。表里的 name 已改为 win.quit。
    NewWindow, QuitExplorer, FolderOptions, MapDrive, UnmapDrive,
    CommandPalette, HotkeyManager, FocusAddress, FocusSearch, Preferences,
    // External
    OpenTerminal, OpenTerminalAdmin, RunCommand, SendKeys,
    // Bookmark
    BmkAddCurrent, BmkShowBar, BmkHideBar, BmkToggleBar,
    // Session
    SessionSave, SessionRestore, SessionDelete, SessionList,
    // 仅目录占位（本工程未实现）
    DupFinder, FileHash, FolderCompare, SearchEverything, OpListDir, OpStat,
    OpTopN, OpDiskUsage, OpSearch, OpReadText, OpNavigate, OpReveal, OpSelect,
    OpOpen, OpListCurrentView, AiTogglePanel, AiStop,
    // ---- 标签个性化（只允许追加在末尾，见 C1 与 commands-baseline.json）----
    // 这三条原本只是右键菜单项；登记成命令是为了满足 R20（人机等价）：
    // 右键弹出来的每个动作都必须有键盘/面板可达路径，否则只有鼠标能做。
    TabRename,         // {name}     改名（空 = 取消改名）
    TabSetColor,       // {color}    0..7（0 = 无颜色）
    TabCloseDuplicates,// 关掉当前标签同路径的其他标签
    TabLock,           // {locked}   锁定/解锁（锁定后关标签族会跳过它）
    TabCopyTitle,      // 复制当前显示名（含改名后的别名）到剪贴板

    // ---- 书签编辑族（只有右键菜单能做 → 补成命令，满足 R20 反向要求）----
    // 这几条原先是书签栏右键菜单里的动作，但是**没有对应命令**：
    //   · 检查器把它们报成"人工可达但无结构化入口"；
    //   · 也就意味着只有鼠标能做 —— 键盘用户、命令面板、AI 都没有等价路径。
    // 语义与菜单项一一对应，参数用书签 id（不是下标：重排后下标会变）。
    BmkEdit,           // {id, name, path}  改名称/路径（path 空 = 变成纯分类）
    BmkRemove,         // {id}              删除一条（可撤销）
    BmkNewFolder,      // {name, parentId?} 新建分类
    BmkUndo,           // 撤销上一次删除/移动/排序
    BmkClearAll,       // 清除全部（可撤销）
    BmkSort,           // {recursive?}   按名称排序
    BmkDedupe,         // 清理重复路径书签
    BmkAdd,            // {path, name?, parentId?} 添加一条（拖放/脚本/AI 的共同入口）
    BmkMove,           // {id, toId} 或 {id, toIndex}  移动（拖拽重排的结构化版本）
    BmkList,           // {parentId?}  返回书签树（**读**：其余 bmk.* 要的 id 从这里来）
    // ---- 设置域（操作软件自身：可枚举 / 可读 / 可写 / 可回读）----
    // 这三条是「AI 调试某个设置项」那条路的入口。没有它们，设置只能是
    // 编译期 struct，机器侧既不知道有哪些项，也没有稳定名字去定位。
    ConfigList,        // 列出全部设置项（key/类型/范围/当前值）
    ConfigGet,         // {key}        读一项
    ConfigSet,         // {key, value} 写一项（带类型与范围校验）
    BmkFocus,          // 焦点进入书签栏（->/<- 选，Enter 开，Shift+F10 菜单，Esc 退回视图）
};

// 命令的风险等级（与 mod 的 risk / 架构规则 R18 对齐）。
// AI 侧据此决定"直接执行 / 必须人工批准 / 一律拒绝"。
enum class Risk {
    Read,     // 只读：不改磁盘、不改视图/选区/焦点
    Write,    // 改状态：切标签、导航、排序、打开面板（可回退，但有副作用）
    Danger,   // 破坏性：删除、覆盖、批量改名（不可轻易回退）
};

// ---------------------------------------------------------------- 参数对象
//
// 命令参数从"一个需要自己拼的字符串"改成"具名对象"（业界共识 P2）。
// 理由不是好看，而是把四件事一次性解决：
//   · 多参数 / 可选参数（字符串做不到）
//   · 类型校验（字符串只能靠 handler 里 _wtoi 猜）
//   · 自动生成 UI 与 MCP inputSchema
//   · 向后兼容地加参数
//
// 内部用 json::Value 存，避免为每种命令定义结构体（127 条命令 → 127 个 struct
// 是过度设计）。取用走具名访问器，拼错键名得到默认值而不是崩溃。
class Args {
public:
    Args() = default;

    static Args FromJson(const json::Value& v) {
        Args a;
        a.v_ = v;
        return a;
    }
    // 空对象：无参数命令、以及"没给参数"的情形。
    static Args Empty() { return Args(); }

    bool IsEmpty() const { return !v_.IsObject() || v_.obj.empty(); }
    const json::Value& raw() const { return v_; }

    bool Has(const wchar_t* key) const { return v_.Find(key) != nullptr; }

    std::wstring Str(const wchar_t* key, const std::wstring& def = std::wstring()) const {
        return Has(key) ? v_.GetStr(key, def) : def;
    }
    int Int(const wchar_t* key, int def = 0) const {
        return Has(key) ? v_.GetInt(key, def) : def;
    }
    bool Bool(const wchar_t* key, bool def = false) const {
        return Has(key) ? v_.GetBool(key, def) : def;
    }

    // 字符串数组参数（如 targets）。没有该键、或不是数组时返回空。
    // 单个元素不是字符串时跳过 —— 宁可少一个目标，也不要塞进去一个坏路径。
    std::vector<std::wstring> StrArray(const wchar_t* key) const {
        std::vector<std::wstring> out;
        const json::Value* a = v_.Find(key);
        if (a && a->IsArray())
            for (const auto& e : a->arr)
                if (e.IsString() && !e.str.empty()) out.push_back(e.str);
        return out;
    }

    // 原样取一个参数（不转型）。给"值类型由数据决定"的命令用 ——
    // 典型是 config.set：value 是布尔还是整数取决于 key，用 Bool/Int 取
    // 会把"给了字符串"静默变成 0/false。返回 nullptr = 没传这个键，
    // 与"传了空值"必须区分（前者该报参数缺失，后者该被注册表校验拒绝）。
    const json::Value* Any(const wchar_t* key) const { return v_.Find(key); }

private:
    json::Value v_;
};

// ---------------------------------------------------------------- 效果描述
//
// 从"单轴风险等级"改成"多个正交特性"（业界共识 P3）。
//
// 为什么单轴不够：`CmdPaste`（写、不可撤销）、`NavRefresh`（写、幂等）、
// `OpenTerminal`（写、作用于进程外）全归到一个 Risk::Write，但安全策略完全不同：
//   · 幂等的操作失败可以安全重试；
//   · 不可撤销的操作必须先确认；
//   · 作用于进程外的操作不能靠"读只读"简单放行。
// 一个枚举表达不了三件事，硬塞进去就只能是"都算危险"或"都算安全"。
//
// 这些是 **hint（声明）而非保证**。MCP 规范明确要求调用方把 annotations
// 视为不可信 —— 所以真正的权限判定还要叠加调用方身份、设置开关与审批，
// 不能只看这里。

// 影响范围：决定"需不需要点名目标"。
enum class Scope {
    CurrentTab,   // 作用于当前标签（导航、排序、视图）
    App,          // 作用于整个窗口（切标签、显隐栏、命令面板）
    System,       // 作用于进程外（启动终端、写注册表、弹系统对话框）
};

struct Effects {
    bool readOnly = false;     // 不改任何状态（不改磁盘、视图、选区、焦点、剪贴板）
    bool destructive = false;  // 可能不可逆：删除、覆盖、批量改名
    bool idempotent = false;   // 同参数重复调用等价（刷新、排序= 是；粘贴、新建= 否）
    bool openWorld = false;    // 作用于进程外：启动程序、访问网络、系统对话框
    bool undoable = false;     // 进撤销栈（我们能撤销的很少：书签、标签关闭）
    Scope scope = Scope::CurrentTab;

    // 由上面几项推导出来的"是否只是读"。不单独存，避免两处真相不一致。
    bool IsPureRead() const { return readOnly && !destructive && !openWorld; }
};

// ---------------------------------------------------------------- 前置条件
//
// "此刻能不能执行"的判定 + 人可读原因（业界共识 P4；Blender 的 poll + disabled hint）。
//
// 为什么必须是**声明式枚举**而不是回调：
//   · UI 要能直接读来灰显按钮并显示 tooltip 原因；
//   · 静态检查脚本要能校验"声明了 needsSelection 就必须有 HasSelection 前置"；
//   · 回调形式没法被枚举、没法被工具化，最后又会变成"每个命令自己写一段 if"。
//
// 组合语义：Requirements 里的项是 **AND** 关系；空集 = 无条件（总能执行）。
// 原因文案按第一个不满足的项给出 —— 同时说三条不满足对用户是噪音。
enum class Req {
    None = 0,
    HasHistoryBack,     // 有可后退的历史
    HasHistoryForward,  // 有可前进的历史
    HasSelection,       // 当前有选中项
    HasTabs,            // 至少有一个标签
    HasClipboard,       // 剪贴板有可粘贴内容
    CanGoUp,            // 上一级有去处（盘符根算有：去此电脑；命名空间/UNC 根没有）
    // 内部用：状态探测还没注册（App 未初始化完），此时不能声称任何命令可执行。
    // 单独一项是为了给出准确原因 —— 借用 HasTabs 会说"没有打开的标签"，
    // 那是误导（标签可能好端端地开着，只是我们还没拿到状态）。
    StateUnavailable,

    // ---- 以下按"边界处必须拒绝"补的（第 8 步）----
    //
    // 为什么需要：这些命令的实现里都有 `if (i > 0) ...` 这类静默防御，
    // 结果是"在边界上按了没反应、也不说为什么"。VS Code 把这种前置条件
    // 声明成 when 子句，菜单项据此灰掉并给出原因（enablement）。
    // 我们照做：声明出来就能在 UI 灰掉、在审计里留下可读原因。
    CanMoveTabLeft,     // 当前标签不在最左
    CanMoveTabRight,    // 当前标签不在最右
    HasClosedTabs,      // 有可重新打开的已关闭标签
    HasMultipleTabs,    // 至少 2 个标签（才谈得上"上一个/下一个/关左侧"）
    HasSelectionSingle, // 恰好一个选中项（属性、打开方式这类只接受单个）
};

struct Requirements {
    // 用固定上限的数组而不是 vector：这是编译期常量表，不需要堆分配，
    // 也让 CommandDef 保持可聚合初始化（constexpr 友好）。
    Req items[4] = {Req::None, Req::None, Req::None, Req::None};

    constexpr bool Empty() const { return items[0] == Req::None; }

    constexpr Requirements() = default;
    constexpr Requirements(Req a) : items{a, Req::None, Req::None, Req::None} {}
    constexpr Requirements(Req a, Req b) : items{a, b, Req::None, Req::None} {}
    constexpr Requirements(Req a, Req b, Req c) : items{a, b, c, Req::None} {}
};

// 人能读懂的原因文案（灰按钮的 tooltip / 机器返回的 error 共用同一份）。
const wchar_t* ReqReason(Req r);

struct CommandDef {
    CmdId id;
    const wchar_t* name;   // 与 capabilities.json 的 id 一致
    const wchar_t* group;  // 机器标识：Tab / Go / Nav / Path / File / View / Window / External / Bookmark / Session
    const wchar_t* scopeUi;  // 中文展示串（标签栏/导航栏/…）。**只用于展示，不用于判定**
    const wchar_t* title;  // 中文显示名
    bool implemented;
    // ---- 结构化元数据（架构规则 R18 + 业界 P1/P3）----
    Effects effects;
    // JSON Schema 字符串，直接复用为 MCP inputSchema / 自动生成 UI。
    // 无参数命令必须显式写 additionalProperties:false（空 schema 语义是"接受任意参数"）。
    const char* argsSchema;
    bool aiExposed = false;      // 是否进 tools/list
    bool needsSelection = false; // 依赖当前选中项（无选中时机器侧应拒绝而非猜测）

    // 前置条件（AND）。空 = 无条件。
    // 判定由调用方通过 CanExecute 完成 —— CommandDef 本身不持有窗口/状态引用，
    // 保持纯数据（可导出、可静态检查、可单测）。
    Requirements requires_;

    // 结构化结果 schema（JSON Schema 字符串）。nullptr = 该命令不产出结果。
    // MCP 规定：给了 outputSchema 就必须按它返回；客户端会校验。
    // 也为空时表示"这个命令只回报成败"，不编造返回值。
    const char* outputSchema = nullptr;

    // ---- 自断言（让"可验证"从脚本层提升到契约层）----
    //
    // 【为什么需要】`ok=true` 只说明"handler 返回了 true"，不说明"效果对吗"。
    // 典型事故：改了内存但没落盘、改了 A 却以为改了 B、被前置条件静默跳过
    // 后仍返回成功。这些都不会让 ok 变 false —— 调用方以为成了，实际没成，
    // 而错误要到很久以后才暴露（那时现场已丢）。
    //
    // 【形态】声明式，不是回调。理由与 Requirements 相同：
    //   · 静态检查能核对"声明了就真能求值"，回调核不了；
    //   · 能导出成文档 / 生成测试用例骨架；
    //   · 不需要每个命令自己写一段 if，避免逻辑散落。
    //
    // 【怎么求值】形如 "sortMode=$arg:by"：
    //   左值取自 exbar/state 的字段名（**必须是可回读的**，见 check_ai_meta 第 17 项），
    //   右值是期望值；`$arg:<key>` 表示"等于入参 key 的值"，
    //   `$arg:<key>.length` 取数组/字符串长度。
    //   执行后由 assert_check::Check 求值，结果进 CallResult.assertions。
    //
    // 【为什么右值要能引用入参】断言"sortMode 等于我刚传的 by"才是真实预期；
    // 写死成 "sortMode=2" 会在换参数时悄悄失效 —— 那种断言比没有更危险，
    // 因为它会一直绿。
    const char* assertion = nullptr;


    // 旧字段的兼容视图：把 effects 映射回三档。仅用于既有代码/AI 白名单的
    // 粗粒度判断，新代码请直接读 effects。
    Risk LegacyRisk() const {
        if (effects.destructive) return Risk::Danger;
        return effects.readOnly ? Risk::Read : Risk::Write;
    }
};

const std::vector<CommandDef>& CommandTable();
const CommandDef* FindCommand(const std::wstring& name);
const CommandDef* FindCommand(CmdId id);

// 命令规格 "Name[:arg]" —— keymap 里 8 档视图模式必须带第几档，
// 否则 Ctrl+Shift+1..8 会全部落到 arg=""（= 第 0 档）。这里是唯一拆分点。
bool SplitCommandSpec(const std::wstring& spec, std::wstring* name, std::wstring* arg);

// 把旧式单字符串参数转成 Args。
//
// 这是**适配层**，只为让既有的 keymap / 序列 / 鼠标规则（都写 "ViewMode:3" 这种
// 单值规格）继续工作。转换规则从命令自己的 argsSchema 推导 —— 取第一个
// required（没有 required 则取第一个 properties）作为键名，按 schema 声明的
// 类型决定存成数字还是字符串。
//
// 关键：这让"哪个键对应这一档参数"只有一份真源（schema），不再需要一张手工
// 维护的映射表（之前 ai_api 里的 kArgKeys 就是缺这个推导而不得不加的补丁）。
// schema 解析失败或无参数时返回空对象。
Args ArgsFromLegacyArg(const CommandDef& def, const std::wstring& legacyArg);

// ---------------------------------------------------------------- 可否执行
//
// 判定一个命令此刻能不能执行，并给出**人可读原因**。
//
// 状态从哪来：App 在启动时注册一个 StateProbe，commands 层不去反向依赖 App。
// 这样判定逻辑可独立单测（喂假状态即可），也不会把窗口句柄带进命令表。
struct CommandState {
    int historyBack = 0;      // 可后退的条数
    int historyForward = 0;   // 可前进的条数
    bool hasSelection = false;
    int selectionCount = 0;   // 选中项个数（HasSelectionSingle 用）
    int tabCount = 0;
    int activeTabIndex = 0;   // 当前标签的位置（判断能不能左右移）
    bool hasClipboard = false;
    bool canGoUp = false;
    bool hasClosedTabs = false;   // 有可重新打开的已关闭标签
};

using StateProbe = std::function<CommandState()>;

// 注册状态探测（App 启动时调一次）。未注册时视为"一切前置都不满足"——
// 失败关闭，宁可不执行也不误判为可执行。
void SetStateProbe(StateProbe fn);

// 返回 true = 可执行。false 时若 reason 非空则填入人可读原因。
// 这是 UI 灰按钮与机器调用前检查的**唯一**入口。
bool CanExecute(const CommandDef& def, std::wstring* reason = nullptr);

// ---------------------------------------------------------------- 返回值
//
// 只读操作要能返回结构化结果（业界 P6）。
//
// 为什么不让所有 handler 都返回 Value：131 条命令里绝大多数是"做了/没做成"，
// 让它们都构造一个 JSON 对象是纯负担，也容易让"写操作假装有返回值"。
//
// 做法：**单独一条注册通道** —— 需要返回值的命令用 RegisterWithResult 注册，
// 其余保持 bool。CommandBus 内部记一个标志，调用方按 hasResult() 判断。
//
// 结果统一是 JSON 对象：便于以后直接序列化成 MCP 的 structuredContent，
// 不需要为每种命令定义 C++ 结构体（那又会变成"只有 C++ 能读懂"的私有格式）。
class CommandBus {
public:
    // handler 收具名参数对象。不再有"字符串 arg"。
    using Handler = std::function<bool(const Args& args)>;
    // 带返回值的 handler。result 由 handler 填，失败时可不填。
    using ResultHandler = std::function<bool(const Args& args, json::Value* result)>;

    static CommandBus& Get();

    void Register(CmdId id, Handler fn);
    // 注册一个会产出结构化结果的命令。同一 id 只能注册一次（后者覆盖）。
    void RegisterWithResult(CmdId id, ResultHandler fn);
    void RegisterByName(const wchar_t* name, Handler fn);

    // 新入口：具名参数。所有新代码都该用它。
    // 返回值仍是"成功/失败"；要拿结构化结果请用 InvokeResult。
    bool Invoke(CmdId id, const Args& args, const wchar_t* actor = L"ui");
    bool InvokeByName(const std::wstring& name, const Args& args, const wchar_t* actor = L"ui");

    // 带结果的调用。result 可为空（只关心成败）；非空时若命令有返回值则填入，
    // 若该命令没有返回值则置为空对象（不编造数据）。
    bool InvokeResult(CmdId id, const Args& args, json::Value* result,
                      const wchar_t* actor = L"ui");
    bool InvokeByNameResult(const std::wstring& name, const Args& args, json::Value* result,
                            const wchar_t* actor = L"ui");

    // 这条命令是否会产出结构化结果。UI/文档侧据此决定要不要展示"结果"区域。
    bool HasResult(CmdId id) const;
    bool HasResult(const std::wstring& name) const;

    // 旧入口（适配层）：单字符串参数。保留是因为 keymap / 序列 / 鼠标规则里
    // 写的是 "ViewMode:3" 这种规格，且它们不关心参数形状。
    // 实现会经 ArgsFromLegacyArg 转成 Args，因此新旧路径最终走同一个 handler。
    bool InvokeLegacy(CmdId id, const std::wstring& arg, const wchar_t* actor = L"ui");
    bool InvokeByNameLegacy(const std::wstring& name, const std::wstring& arg,
                            const wchar_t* actor = L"ui");

    // 最近一条命令（状态栏/日志用）
    std::wstring lastCommand() const { return last_; }

    // 最近一次"为什么没执行"。空串 = 上次调用没有被前置条件拦下。
    // 存在这里有两条理由：
    //   · UI 要能给出"这个按钮为什么是灰的"（与灰显同源，不另算一遍）；
    //   · AI/脚本侧要能回报真实原因，而不是笼统的"命令执行失败"。
    // 以前 Dispatch 算出了 reason 却只写进日志与审计，调用方看不到，
    // 于是对外只能报"执行失败"，把问题留给了猜。
    const std::wstring& lastBlockReason() const { return lastBlockReason_; }

    // 由 handler 主动给出"我为什么没做成"。
    //
    // 前置条件拒绝（CanExecute）有 reason，可 handler 内部失败（key 不存在、
    // 类型不符、路径为空……）以前只能 return false，原因烂在日志里，调用方
    // 拿到的是笼统的"执行失败"。调用方因此无法决策 —— 名字打错和值超范围
    // 该做的下一步完全不同。
    //
    // 约定：handler 在 return false 之前调用；Dispatch 随后把它搬到
    // lastBlockReason_，于是 UI / AI / 审计看到的是同一句话。
    // 返回 true 的 handler 不需要调用（残留会由下次 Dispatch 开头清空）。
    void NoteFailure(const std::wstring& reason) { pendingReason_ = reason; }

private:
    CommandBus() = default;
    bool Dispatch(CmdId id, const Args& args, json::Value* result, const wchar_t* actor);
    void Audit(const CommandDef& def, const std::wstring& argDesc, const wchar_t* actor, bool ok);

    std::vector<std::pair<CmdId, Handler>> handlers_;
    std::vector<std::pair<CmdId, ResultHandler>> resultHandlers_;
    std::wstring last_;
    std::wstring lastBlockReason_;
    std::wstring pendingReason_;   // handler 报的原因，本次 Dispatch 内消费
};
