# -*- coding: utf-8 -*-
"""AI 元数据静态对账（架构规则 R18 / R20）。

检查命令表与实现是否自洽 —— 这些错误在运行时只会表现为
"AI 说工具不存在"或"参数被静默丢弃"，很难查：

  1. 每个 aiExposed 的命令：implemented 必须为 true
  2. 每个 aiExposed 的命令：argsSchema 必须是合法 JSON 对象
  3. schema 声明的参数必须能在 handler 里找到取用（args.Int/Str/Bool("<键>")）
     —— 否则就是"声明了参数但没实现"，调用方传了等于白传
  4. needsSelection 的命令暴露给 AI 的前提：观测面可读选中
     （state 快照输出 selectionCount）。旧规则"一律不暴露"基于观测面不存在
     时"AI 无法指定目标"；观测面落地后人机协作语义成立——用户在界面选中
     （人表达意图），AI 先读 selection 确认目标、再调用命令作用于它。
     观测面一旦缺失（被删/断链）立即恢复 ERROR。
  5. 无参数命令的 schema 应显式写 additionalProperties:false
     （MCP 规范：空 schema 的语义是"接受任意参数"，是不安全的默认值）
   ...
 15. 人机等价（R20）：每个 aiExposed 命令都必须有人工可达路径，
     且**带必填参数的命令必须有一条路径真能把参数递进去**

不需要编译，纯文本对账。用法：python tools/check_ai_meta.py

## 为什么解析要按结构切、不按正则匹配整行

早先的写法是一条正则匹配"整行条目"，其中 argsSchema 位置被钉死成
`"..."` 字符串字面量。2026-11-20 把 119 条无参 schema 提取成 `kNoArgs`
常量后，整行正则一条也匹配不上（140 条只命中 21 条），而脚本仍然打印
"全部通过" —— 检查面静默缩水 85%，分母从 140 掉到 21，没有任何报错。

所以现在改成：
  · 定位 `kTable` 初始化列表，按顶层花括号切条目、按顶层逗号切字段；
  · 字段值可以是字符串字面量，也可以是常量名（`kNoArgs`），常量再去
    `constexpr const char* NAME = ...` 的定义处取值；
  · 额外硬断言"解析出的条目数 == CmdId:: 出现次数"，任何解析退化当场报错。

这样以后表里换任何常量名、加任何新字段，都不会再让检查静默失效。
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def ai_server_source():
    """AI 通道的实现可能按职责分片；对账时仍按一个逻辑模块看。

    2026-11-24 把协议分派拆到 ai_server_rpc.cpp 后，下面这些"通道里有没有
    events/since / traceparent"的判据不能只看传输层那一片，否则文件拆了、
    行为没变，检查却红成"能力消失"。这里显式拼接两片，判据不放松。
    """
    parts = []
    for rel in ('src/ai_server.cpp', 'src/ai_server_rpc.cpp'):
        path = ROOT / rel
        if path.exists():
            parts.append(path.read_text(encoding='utf-8', errors='replace'))
    return '\n'.join(parts)

COMMANDS = ROOT / 'src/commands.cpp'
# 命令表本体按作用域分在两个文件里（用户操作域 / 工具·Agent 域），
# 共用的 schema/豁免/效果常量在 commands_table.h。
TABLE_SRCS = [ROOT / 'src/commands_table.cpp',
              ROOT / 'src/commands_table_tools.cpp']
TABLE_CONSTS_SRC = ROOT / 'src/commands_table.h'
HANDLERS_GLOB = 'app_commands*.cpp'
KEYMAP_JSON = ROOT / 'data/keymap.json'
INPUT_ROUTER = ROOT / 'src/input_router.cpp'
KEYMAP_CPP = ROOT / 'src/keymap.cpp'

# AI 协议实现的源文件分片。
#
# 【为什么按 glob 收集，而不是写死 ai_api.cpp】
# 2026-11-22：ai_api.cpp 把"执行命令"与"写 JSON"拆开（序列化搬到
# ai_api_json.cpp）。检查器因为写死读 ai_api.cpp，**119 条检查全部失效**：
# 报"bookmarkTotalCount 不在 exbar/state 里"，而那个字段好好地写在
# ai_api_json.cpp 的 StateToJson 里。字段一个没少，检查面却没了 —— 与
# 文件头记的那次"整行正则只命中 21/140 却打印全部通过"是同一类事故：
# **检查面静默缩水**。
#
# 分片只会继续增加（每次按职责重构都可能拆），所以检查面必须跟着**代码**走，
# 而不是跟着文件名走：这里按 glob 收齐所有分片，函数定位则按"找到定义体"
# 来做（见 _ai_func_body）。
AI_SRC_GLOB = 'ai_*.cpp'

g_fail = 0

# 已知设计债务：这些破坏性命令目前只能作用于"当前选中项"，没有目标参数。
# 机器因此无法安全调用它们（不知道用户此刻选了什么），所以它们也都不在
# aiExposed 白名单里。修法是给它们加 targets 参数（docs/OPERATION-MODEL.md §4.3），
# 加完后从本表移除。这里显式登记而不是放宽规则 —— 让债务可见、可追踪。
# 参数取用发生在别处的命令：handler 只做转发，真正的参数读取在公共实现里。
# 静态扫描只能看到 handler 函数体，看不到被调用的函数内部，所以这些必须显式登记。
# 登记后本脚本不再要求"schema 里的键出现在 handler 里"，但**会额外检查**
# 那个公共实现确实读了参数（见 INDIRECT_ARGS 的 docstring 说明）。
INDIRECT_ARGS = {
    # FsCopyTo/FsMoveTo 的 handler 只有一行 return TransferTargets(args, ...)，
    # 参数在 App::TransferTargets 里读（src/app_commands.cpp）。
    'fs.copy': 'App::TransferTargets',
    'fs.move': 'App::TransferTargets',
    # OpTopN/OpDiskUsage 的 regR 只是一行转发，参数在 RegisterScanCommands
    # 里的 scanImpl lambda 中读取（两条共用同一份实现）。
    'tool.topn': 'RegisterScanCommands::scanImpl',
    'tool.diskusage': 'RegisterScanCommands::scanImpl',
}

KNOWN_NO_TARGET = {
    'file.cut', 'file.delete', 'file.copy.to', 'file.move.to', 'file.rename.batch',
    'path.copy.link',
    # Shift+Delete 的人用入口：与 file.delete 同一份"当前选中"设计债务，
    # 同样 aiExposed=false。机器侧要永久删必须走 fs.delete + approve，
    # 不能靠这条无目标参数的键盘命令来表达。
    'file.delete.permanent',
}

# R20 的判定分两条路，因为"可达"有两种不同含义：
#
#   A. **无必填参数**的命令 → 命令面板（Ctrl+Shift+P）列出全部已实现命令并
#      以 Args::Empty() 调用，人点得到、点了能成。这条是**机制保证**，不需要
#      逐条登记：只要 implemented=true 就自动满足。palette_covers() 负责判定。
#
#   B. **有必填参数**的命令 → 命令面板那条路点了必然失败（拿不到参数），
#      所以必须另有一条真能把参数递进去的路：热键内联参数（"view.mode:3"）、
#      带参的源码调用，或者本表登记的界面路径。
#
# 本表只为 B 里"参数来自界面而不是来自命令规格"的那些准备。写清三件事：
# 界面位置、触发动作、参数从哪儿来。只写"菜单里有"不算 —— 那只证明菜单存在，
# 不证明能力等价。
HUMAN_PATH = {
    'tab.rename': '标签右键菜单「重命名标签页...」→ App::ShowTabMenu(kTabMenuRename) '
                  '→ PromptText（参数 name 来自输入框）→ App::RenameTab',
    'tab.color': '标签右键菜单「设置标签页颜色」→ 二级色板（参数 color 来自选中的那一行，'
                 'index 来自被右键的标签）→ CommandBus::Invoke(CmdId::TabSetColor) '
                 '→ App 的 tab.color handler → App::SetTabColor',
    'tab.activate': '鼠标点标签栏任意标签（TabStrip 命中测试 → NotifySelect），'
                    '参数 index 就是被点中的那一格',
    'fs.open': '文件视图双击条目 / 回车（参数 path 即被双击的项）',
    'fs.open.tab': '文件视图 Ctrl+回车 / 中键（参数 path 即被点的项）',
    'fs.open.window': '文件视图 中键 / 右键「在新窗口中打开」（参数 path 即被点的项）',

    # 书签的结构化参数由拖放/右键给出 —— 与 fs.open 那批同理：
    # 人用"拖进去"，机器用"点名 path"，同一份实现（App 的 BmkAdd handler）。
    'bmk.add.path': '从资源管理器把目录拖到书签栏（参数 path 即被拖入的目录），'
                    '或右键「添加当前目录为书签」；两者最终都调 BmkAdd',
    'bmk.move': '在书签栏上拖拽重排 / 拖进分类（参数 id 是被拖的那条，'
                'toId/toIndex 是落点），DropTargetAt 算出落点后调 BmkMove',

    # 下面这些是"确定式双胞胎"：机器用参数点名目标，人用"先选中、再执行"。
    # 同一个能力两条入口，参数来源不同但结果等价 —— 这正是第 4 步
    # （交互与确定分离）的设计，不是缺口。
    'fs.rename': 'F2 → CmdRename（参数 target 取当前选中项，newName 来自系统改名框）；'
                 '确定式版由 shell_ops::RenameTo 实现同一件事',
    'fs.delete': 'Delete 键 → CmdDelete（参数 targets 取当前选中集合）；'
                 '确定式版由 shellops::SelectByPaths + DeleteSelection 实现同一件事',
    'fs.copy': 'Ctrl+Shift+A → CopyToFolder（参数 targets 取选中集合，to 来自文件夹选择框）；'
               '确定式版由 App::TransferTargets 实现同一件事',
    'fs.move': 'Ctrl+Shift+X → MoveToFolder（同上，move=true）',
    'fs.copy.toclip': 'Ctrl+C → CmdCopy（参数 targets 取选中集合，写同一份剪贴板）',
    'fs.cut.toclip': 'Ctrl+X → CmdCut（同上，带 DROPEFFECT_MOVE）',
    # 属性没有独立热键（Alt+Enter 未绑定，核实过 keymap.json 与 kExtra 都没有）。
    # 真实入口是状态栏"选中容量"段：点它 = 对选中项看属性（app.cpp 的 SetClickFn）。
    # 前提是该段启用（statusSegs 默认含 Seg_Size=5）。
    'fs.properties': '状态栏「选中容量」段点击 → CmdProps（参数 target 取选中项）；'
                     '默认配置 statusSegs={0..6} 含该段。注意没有 Alt+Enter 绑定',
    'dev.rollback': '命令面板选中后先弹破坏性确认，再按 schema 追问 path（与 AI 侧 approve 等价）',
    'tool.stat': '状态栏「选中容量」段 + 详细信息视图列展示同一组字段'
                 '（名称/大小/类型/隐藏/只读）',
    # 清除历史：人在地址框下拉（历史/最近访问）底部点「清除历史记录」，
    # 与命令走同一个 App::ClearHistory（不是两套实现）。
    'nav.history.clear': '地址框下拉底部「清除历史记录」→ NavBar clearHistory_ '
                         '→ App::ClearHistory（与命令同一实现）',

    # 文件集工具三条（2026-11-21）：设计是"空参数 = 取界面现状，带参数 = 机器点名"，
    # 所以 hash / dupfinder 无必填参数、走命令面板自动满足；compare 有两个必填路径，
    # 人由两框输入给出（左侧预填当前目录），结果统一进只读报告窗口（report.cpp）。
    # 批量重命名：参数只有一个模式串，人就是"按一下、填个模式"。
    "file.rename.batch": "Ctrl+Shift+R（或命令面板）→ PromptText 问命名模式"
                         "（初值 {name}_{nn}，可选 {ext}/{old}/{n}/{nn}/{nnn}）→ "
                         "MessageBox 预览前 20 条「旧 → 新」并确认 → "
                         "逐项 shellops::RenameTo（FOF_ALLOWUNDO，Ctrl+Z 可退）",
    'tool.compare': '命令面板 → tool.compare：弹 PromptText2 两个输入框'
                    '（左=当前目录预填，右=手工填或粘贴目标目录），确认后调 CompareFolders；'
                    '结果弹只读报告窗口（report.cpp），可 Ctrl+A / Ctrl+C 取走',

    # 设置面板（导航栏右键 →「设置…」，动作号 900；win.preferences 命令走同一扇面板）：
    # 列出键表里每一项与当前值。
    # 值在渲染时直读键表（一屏几十行，不能每行发一次命令），但**选中某项后**
    # 走 config.get 读最新值、config.set 写回 —— 与 AI 完全同一条路径、同一份校验。
    # 参数 key 就是被点中的那一行的键名（settings.json 里的名字）。
    'config.get': '导航栏右键「设置…」→ 设置面板（settings_panel.cpp）：'
                  '点某一项时先调 config.get 读最新值（参数 key 即该行的键名）',
    'config.set': '导航栏右键「设置…」→ 设置面板：布尔项点一下翻转、数值/字符串弹输入框、'
                  '数组填逗号分隔，一律调 config.set（参数 key/value 来自该行与输入框）',

    # 全量覆盖 Batch 2：三条带路径的确定式命令，人侧都由菜单/对话框递参数。
    'bmk.import': '书签栏右键「导入书签...」→ BmkImport 处理器的人侧分支：文件对话框给 path，'
                  '自绘菜单给 mode（0 追加 / 1 覆盖），覆盖前明确确认；'
                  '机器侧直接带 {path, mode} 调同一条命令',
    'bmk.export': '书签栏右键「导出书签...」→ BmkExport 处理器的人侧分支：'
                  '文件保存对话框给 path，写出后弹已导出条数；机器侧直接带 path',
    'clipboard.copy': '书签栏右键「复制路径」/ 面包屑段右键「复制此路径」→ '
                      'clipboard.copy（参数 text 来自该项/该段路径）',
    'ext.terminal.at': '面包屑段右键「在此打开终端」→ ext.terminal.at（参数 path 来自该段路径）',

    # 搜索域（导航栏搜索框的两条带）。参数都来自界面，不是来自命令规格：
    #   · 范围档位 = 菜单里点的那一行；自定义档的 path = 弹出的输入框
    #   · 搜索词   = 历史下拉里点的那一条
    'search.scope.set': '导航栏搜索框左侧「搜索范围」菜单 → 选中一档（自定义档会再问一次目录）'
                        '→ App::ShowSearchScopeMenu → 命令总线 search.scope.set',
    'search.query.set': '导航栏搜索框右侧「搜索历史」下拉 → 选中一条历史 → 命令总线 search.query.set'
                        '（参数 query 即那条历史词；**只回填不执行搜索**）',
}

# 机器专用查询：人能通过命令面板触发，但拿到的是 JSON（写进审计日志），
# 界面上没有可见结果。它们**不是**违规 —— 这类命令存在的意义就是把结构化
# 数据交给 agent（第 5 步的 OpListDir/OpStat 就是为此而加）。登记在这里
# 是为了让"人点了看不到东西"这件事是已知的，而不是被当成 bug 反复排查。
AI_ONLY_QUERIES = {
    'tool.list': '返回当前目录的条目数组；人的等价物是直接看文件列表',
    # tool.topn / tool.diskusage 以前也在这张表里（"界面上没有按体积排序的视图"）。
    # 2026-11-22 起它们有了人工路径：长任务搬去工作线程后，人侧会弹进度窗口，
    # 跑完再弹一份只读报告 —— 所以它们不再"只有 agent 看得到结果"，从表里摘掉。
    #
    # 作业域这三条则相反：人能触发（命令面板），但**看不到结果**
    # —— 作业状态只在返回值里，而命令面板丢弃返回值。
    'tool.job.get': '返回作业状态与结果；作业状态不在界面上（进度窗口只在跑的那一瞬存在）',
    'tool.job.cancel': '取消正在跑的作业；人在进度窗口上按"取消"才是有人可见反馈的等价路径',
    'tool.job.list': '列出全部作业；界面上没有作业列表',
    # P0-9：dev.diff 人也能在命令面板触发，但结构化 diff 只写进审计/返回值，
    # 面板本身不渲染 patch —— 它的价值就是给 agent 审阅"这一轮改了什么"。
    'dev.diff': '返回工作树的结构化 diff（文件+SHA-256+mtime+patch）；命令面板不渲染 patch，结果给 agent 审阅',
}

# 同义命令配对：两条命令做的是**同一件事**，走的是同一批底层实现
# （shellops::DeleteSelection / RenameTo / TransferToFolder / CopyPaths），
# 只是入口不同 —— `file.*` 作用于用户当前选中，`fs.*` 由参数点名目标。
#
# 【为什么它们必须声明同一份 Effects】
# 历史上它们各抄了一份，于是同一个事实有两个源头，而且**暴露给 AI 的那份
# 恰好是更乐观的那份**：
#     fs.delete   undoable=1（实际没有撤销栈）
#     fs.move     destructive=0（移动确实让源文件消失）
#     fs.copy     destructive=0 而 file.copy.to destructive=1（同一件事，相反）
# AI 依据这些声明决定"敢不敢试错"，声明错了它就是**基于错的认知**行动。
#
# 修法不是把两份改成一样（下一个人加命令时又会分叉），而是引用同一份
# Effects 常量 —— 编译期就绑死。本表是那条约束的**兜底**：即使有人绕开
# 常量直接写了字面量，这里也会红。
#
# 这张表同时是"确定式版本覆盖了哪些交互式命令"的唯一登记处
# （第 9 项的覆盖进度、第 24 项的契约一致性都用它）——
# 两处各写一份 inevitably 会分叉。
SAME_OPERATION_PAIRS = {
    'file.rename': 'fs.rename',
    'file.delete': 'fs.delete',
    'file.copy.to': 'fs.copy',
    'file.move.to': 'fs.move',
    'file.open': 'fs.open',
    'file.open.tab': 'fs.open.tab',
    'file.open.window': 'fs.open.window',
    'file.cut': 'fs.cut.toclip',
    'file.copy': 'fs.copy.toclip',
    'file.properties': 'fs.properties',
}

# 真实撤销机制登记表：命令名 → 撤销栈在哪（供人核对）。
#
# 【为什么要有这张表】
# `undoable=1` 是**给 AI 看的承诺** —— 它据此决定"敢不敢试错"。
# 2026-11-21 实测：全表 24 条声明 undoable，其中只有 13 条 bmk.* 真的有栈，
# 其余 11 条（tab.move.* / tab.pin / file.undo / session.named.*）全是空承诺。
# 空承诺比不声明更坏：AI 会以为"错了能退"，于是放心地做破坏性尝试。
#
# 【判据】声明 undoable=1 的命令必须在这张表里；表里的命令也必须真的声明了
# undoable=1 —— 两处必须同步。登记表不是"允许清单"，是**事实清单**：
# 往里加一行 = 承诺"这里真的有栈"，那就要真的去实现一个栈。
UNDO_MECHANISM = {
    # 书签：BookmarkStore::undo_（整树快照栈，上限 20，带 actor 标签）
    'bmk.add.current': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.edit': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.remove': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.newfolder': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.undo': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.clear': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.sort': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.dedupe': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.add.path': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.add.selection': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.add.tabs': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.copy': 'BookmarkStore::undo_（快照栈 ≤20）',
    'bmk.move': 'BookmarkStore::undo_（快照栈 ≤20）',
    # 解散分类：改前整树快照（分类消失 + 子项提层是一次结构变动，
    # 快照一次覆盖全部效果，不需要"逆操作"那套差集推导）。
    'bmk.dissolve': 'BookmarkStore::undo_（快照栈 ≤20）',
    # 导入两种模式都进快照：覆盖走 ReplaceAll，追加走 Merge（Batch 2 补了 PushUndo）。
    'bmk.import': 'BookmarkStore::undo_（快照栈 ≤20；import=0 追加 / 1 覆盖）',
    # 标签：core::TabSet 的关闭历史（tab.reopen 就是还原路径）
    'tab.close': 'core::TabSet 关闭历史（tab.reopen 还原）',
    'tab.close.duplicates': 'core::TabSet 关闭历史（tab.reopen 还原）',
    # 文件：App::fileUndo_（2026-11-21 新增，src/file_undo.h）
    'file.rename': 'App::fileUndo_（改名逆操作：交互式改名靠目录清单差集推新旧名）',
    'file.rename.batch': 'App::fileUndo_（改名逆操作：旧名→新名）',
    'file.copy.to': 'App::fileUndo_（复制逆操作：撤销=删副本）',
    'file.move.to': 'App::fileUndo_（移动逆操作：源→目标）',
    'fs.rename': 'App::fileUndo_（改名逆操作：旧名→新名）',
    'fs.move': 'App::fileUndo_（移动逆操作：源→目标）',
    'fs.copy': 'App::fileUndo_（复制逆操作：撤销=删副本）',
}

# 界面上存在的动作 → 覆盖它的命令别名（None = 已知缺口，待补命令）。
#
# 【为什么需要这张表】前 15 项检查的是 **AI → 人**：暴露出去的每条命令
# 人都得能调（R20）。反方向 **人 → 结构化** 从来没查过 —— 于是"右键菜单里
# 写了就完事"的动作会变成 UI 私有代码：快捷键绑不上、命令面板搜不到、
# 外部脚本与 AI 都够不着，而且**没有任何检查会报**。
#
# 补一条命令时把这里的 None 换成别名；改命令名时这里会立刻红（别名找不到），
# 不会出现"登记过 = 问题解决了"的假象。
#
# 实测依据：这 5 条 Bookmark 动作的 store 调用（Remove / AddFolder /
# ReplaceAll / Undo）在 bookmark_bar.cpp 之外**零调用点**，命令层根本够不到。
# 可写的界面状态 → 必须能从 exbar/state 回读（第 17 项的判据表）。
# 左边是 StateSnapshot 字段名（同时也是 StateToJson 的 JSON 键），右边写清
# "哪条命令会改它" —— 这样报错时能直接指出该补哪条链路。
WRITABLE_STATE = [
    ('sortMode', 'view.sort'),
    ('groupBy', 'view.group'),
    ('bookmarkBarVisible', 'bmk.show / bmk.hide / bmk.toggle'),
    ('navPaneVisible', 'view.toggle.nav'),
    ('previewPaneVisible', 'view.toggle.preview'),
    # 搜索态：search.find 改它。没有回读时"搜索成功"只能信命令自己的回执，
    # 而宿主视图不换页也会返回 ok（2026-11-21 实测的假成功就是这样漏掉的）。
    ('searching', 'search.find'),
    ('searchQuery', 'search.find'),
    # ---- 以下是第 1 档第 2 步（P0-4）补的断言回读面 ----
    # 每个都是"某条命令能改，但改完读不回来"的那一类。
    # 它们的共同点：以前这类命令**只能靠命令自述**证明自己生效了，
    # 而命令自述正是假绿的高发区。
    ('tabCount', 'tab.new / tab.close / tab.clone / tab.reopen'),
    ('activeTabTitle', 'tab.rename（派生显示名）'),
    ('activeTabAlias', 'tab.rename（被写入的原值）'),
    ('activeTabIsLast', 'tab.activate.last'),
    ('bookmarkCount', 'bmk.add / bmk.remove（叶子口径）'),
    ('bookmarkTotalCount', 'bmk.add / bmk.remove / bmk.newfolder（节点口径）'),
    ('sessionSaved', 'session.save / session.delete'),
    ('namedSessionCount', 'session.named.save / session.named.delete'),
    ('showHiddenFiles', 'view.toggle.hidden'),
    ('showSystemFiles', 'view.toggle.system'),
    ('showExtensions', 'view.toggle.extensions'),
    ('shellHiddenState', 'view.cycle.hidden'),
    # ---- 搜索域（导航栏搜索框的范围菜单 / 历史下拉）----
    # 这四个是"某条命令能改，但改完读不回来"的那一类：范围菜单与历史下拉
    # 过去只有界面一条路，AI 看不见当前配的是什么，也读不出搜过什么。
    ('searchRange', 'search.scope.set'),
    ('searchRangePath', 'search.scope.set'),
    ('searchHistoryCount', 'search.history.clear'),
    ('searchInput', 'search.query.set'),
]

MENU_ACTIONS = {
    # ---- 标签右键菜单（tab_strip_actions.cpp 的 ui::Menu 链）----
    '标签·新建标签页': 'tab.new',
    '标签·关闭': 'tab.close',
    '标签·关闭其他': 'tab.close.others',
    '标签·关闭左侧': 'tab.close.left',
    '标签·关闭右侧': 'tab.close.right',
    '标签·关闭重复标签页': 'tab.close.duplicates',
    '标签·重命名标签页': 'tab.rename',
    '标签·设置标签页颜色': 'tab.color',
    '标签·复制标签': 'tab.clone',
    '标签·复制路径': 'path.copy',
    '标签·锁定': 'tab.lock',
    '标签·复制标签名': 'tab.copy.title',
    '标签·标签切换器': 'tab.switcher',
    '标签·向左移动': 'tab.move.left',
    '标签·向右移动': 'tab.move.right',
    '标签·移动到开头': 'tab.move.first',
    '标签·移动到末尾': 'tab.move.last',
    '标签·移到新窗口': 'tab.move.window',
    '标签·移到新窗口（不迁移历史）': 'tab.move.window',
    '标签·固定标签': 'tab.pin',
    '标签·取消固定': 'tab.pin',
    '标签·重新打开关闭的标签': 'tab.reopen',
    # ---- 视图菜单（nav_bar_actions.cpp）----
    '视图·选择详细信息…': 'view.columns.choose',
    '视图·图标大小…': 'view.zoom.choose',
    # 排序/分组二级菜单：文案带方向与当前维度后缀，按前缀归一到同一命令。
    '视图·排序方式': 'view.sort',
    '视图·分组依据': 'view.group',
    '视图·设置…': 'win.preferences',
    '视图·在新标签页打开': 'tab.new',
    '视图·在新窗口打开': 'fs.open.window',
    '视图·属性': 'fs.properties',
    # ---- 书签右键菜单（bookmark_bar.cpp 的 ShowItemMenu）----
    '书签·添加当前目录为书签': 'bmk.add.current',
    '书签·新建分类': 'bmk.newfolder',
    '书签·撤销删除': 'bmk.undo',
    '书签·按名称排序（含分类内）': 'bmk.sort',
    '书签·清理重复书签': 'bmk.dedupe',
    '书签·清除全部书签': 'bmk.clear',
    '书签·隐藏书签栏': 'bmk.hide',
    '书签·编辑': 'bmk.edit',
    '书签·删除': 'bmk.remove',
    # ---- 分类结构重组（bookmark_bar_popup.cpp / bookmark_category_popup.cpp）----
    # 解散分类：把分类里的直接子项提到上一层（顶层分类 → 书签栏）。
    '书签·解散分类': 'bmk.dissolve',
    # 排序本分类：只排这个分类内的直接子项（bmk.sort 带 parentId）。
    '书签·排序本分类': 'bmk.sort',
    # 分类内的三种移动都是同一条 bmk.move，只是 toId/toIndex 不同。
    '书签·上移': 'bmk.move',
    '书签·下移': 'bmk.move',
    '书签·移到书签栏': 'bmk.move',
    # ---- 全量覆盖 Batch 2：菜单直接调命令（不再是 UI 私有动作）----
    # 「打开」用现成的 fs.open：书签项拿到的是路径，机器侧也从 bmk.list 拿路径。
    '书签·打开': 'fs.open',
    '书签·复制路径': 'clipboard.copy',
    '书签·重新载入书签文件': 'bmk.reload',
    '书签·导出书签': 'bmk.export',
    '书签·导入书签': 'bmk.import',
    '视图·复制此路径': 'clipboard.copy',
    '视图·在此打开终端': 'ext.terminal.at',
    # 导入模式弹层里的两行也归到同一条命令（mode 值不同）。
    '书签导入·追加到现有书签（推荐）': 'bmk.import',
    '书签导入·覆盖全部书签（现有书签将被替换）': 'bmk.import',
    # ---- 以下四条是"命令已存在、菜单也调了命令，但过去漏登记"的项 ----
    '书签·复制书签': 'bmk.copy',
    '书签·收藏所有标签页': 'bmk.add.tabs',
    '书签·添加当前目录': 'bmk.add.current',
    '视图·清除历史记录': 'nav.history.clear',
    '状态栏·复制当前路径': 'path.copy.dir',
    '状态栏·隐藏状态栏': 'config.set',
    # ---- 搜索框的两条带（app_search.cpp 的 ui::Menu 链）----
    # 范围菜单的四个档位是同一条命令的四个参数值；自定义档的路径来自输入框。
    '搜索·当前目录（仅本层）': 'search.scope.set',
    '搜索·当前目录（含子目录）': 'search.scope.set',
    '搜索·此电脑': 'search.scope.set',
    '搜索·自定义路径…': 'search.scope.set',
    '搜索·清除搜索历史': 'search.history.clear',
    # ---- 导航窗格空白区右键（nav_pane_shell.cpp 的 ui::Menu 链）----
    '导航·刷新': 'nav.refresh',
    '导航·展开所有文件夹': 'nav.tree.expand.all',
    # 「（暂无搜索历史）」是禁用提示行，不是动作（点它什么也不发生）。
    # 这类行单列在 MENU_DISABLED_HINTS：它们既不是缺口、也不该进覆盖计数 ——
    # 混进"待做"清单里只会把真缺口冲淡。
    '搜索·（暂无搜索历史）': None,
}

# 菜单里的**禁用提示行**：文案存在、动作号 < 0、恒为 disabled，
# 点下去什么也不发生。它们不是"人工可达但无结构化入口"的缺口，
# 而是空态说明。单独一类，供第 16 项跳过、并在输出里点名。
#
# 【为什么必须显式登记而不是靠动作号猜】动作号 < 0 也可能表示别的约定
# （标签右键的「最近关闭」标题行是 -1、导入弹层的「完成」是 -2）。
# 判据只看"这一行的点下去有没有语义"，那件事只有写菜单的人知道。
MENU_DISABLED_HINTS = {
    '搜索·（暂无搜索历史）',
}


def fail(msg):
    global g_fail
    print(f'  ERROR  {msg}')
    g_fail += 1


def ok(msg):
    print(f'  ok     {msg}')


# ---------------------------------------------------------------- 结构解析
#
# kTable 是一串 `{...},` 条目，每个条目的字段按 CommandDef 的声明顺序排列：
#   id, name, group, scopeUi, title, implemented, effects,
#   argsSchema, aiExposed, needsSelection [, Requirements{...}] [, outputSchema]
#
# 字段值可能含逗号（Effects{0, 0, ...}）与花括号（Requirements{...}），
# 所以必须深度感知地切，不能用 split(',')。

def _strip_comments(text):
    """去掉 // 行注释与 /* */ 块注释，但不动字符串里的内容。

    必须做：表里有多行注释块，注释里出现的 `{`/`}` 会把深度算错。
    """
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def _split_top_level(text, sep=','):
    """按顶层分隔符切分，跳过 {} () [] 内部与字符串内部。"""
    parts, cur = [], []
    depth = 0
    in_str = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            cur.append(c)
            if c == '\\' and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c in '{([':
            depth += 1
            cur.append(c)
        elif c in '})]':
            depth -= 1
            cur.append(c)
        elif c == sep and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    tail = ''.join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def _html_block(text, start):
    """从 start（指向 '{'）取出配平的一段，返回 (内容, 结束位置)。"""
    depth = 0
    in_str = False
    i = start
    while i < len(text):
        c = text[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i], i + 1
        i += 1
    return None, i


def _string_constants(text):
    """收集 `constexpr const char* NAME = R"(..)"` / `= ".."` 这类常量定义。

    这是让 argsSchema 位置可以写常量名的前提（kNoArgs 就是这么来的）。
    """
    consts = {}
    for m in re.finditer(
            r'constexpr\s+const\s+char\*\s*(\w+)\s*=\s*(R"\((.*?)\)"|"((?:[^"\\]|\\.)*)")',
            text, re.S):
        name = m.group(1)
        # 【修】以前这里判的是 group(2)（整个"字面量"分支），于是**普通字符串**
        # （`= "..."`）也被当成 raw 分支，取到 group(3)=None ——
        # 常量表里存的是 None，任何引用它的字段都解析不到值。
        # 只有 R"(...)" 形式（kNoArgs）才恰好能用，所以这个缺陷一直没被发现：
        # 全表只有那一个 raw 字符串常量。
        # 现在按"哪个分支真的匹配了"来判断：group(3) 有值 = raw 形式。
        if m.group(3) is not None:
            consts[name] = m.group(3)
        else:
            consts[name] = (m.group(4) or '').replace('\\"', '"').replace('\\\\', '\\')
    return consts


def _string_literal(field, consts):
    """把字段值解成字符串：字面量直接解，常量名查表。解析不到返回 None。"""
    field = field.strip()
    if field.startswith('"'):
        m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', field)
        if not m:
            return None
        return m.group(1).replace('\\"', '"').replace('\\\\', '\\')
    if field.startswith('R"('):
        m = re.fullmatch(r'R"\((.*)\)"', field, re.S)
        return m.group(1) if m else None
    return consts.get(field)


def _wstring(field):
    """解 L"..." 形式的宽字符串字面量。"""
    m = re.fullmatch(r'L"((?:[^"\\]|\\.)*)"', field.strip())
    return m.group(1) if m else None


def _body_of(text, name, kind='struct'):
    """取 `kind name { ... }` 的函数体/结构体体（用于读 Requirements 之类）。"""
    idx = text.find(name)
    if idx < 0:
        return ''
    brace = text.find('{', idx)
    if brace < 0:
        return ''
    body, _ = _html_block(text, brace)
    return body or ''


# ------------------------------------------------------- AI 协议分片的定位
#
# 这一组函数的唯一职责：**不让检查面跟着文件名走**。
#
# 判据是"这段代码还在不在这套协议里"，不是"它还在不在那个文件里"。
# 每一条都返回"全部命中"或"没命中"，绝不返回半截 —— 半截会让检查项
# 看起来跑过了，实际只覆盖了一部分。

def _ai_source_paths():
    """AI 协议实现的源文件（按 glob 收齐，不写死文件名）。"""
    return sorted((ROOT / 'src').glob(AI_SRC_GLOB))


def _ai_all_text():
    """所有 AI 协议分片的源码文本（去注释后合并）。

    合并而不是逐文件看：检查项关心的都是"这个字段/函数在不在这套协议里"，
    分片怎么拆都不影响覆盖面。
    """
    global _AI_TEXT
    if _AI_TEXT is None:
        chunks = []
        for p in _ai_source_paths():
            chunks.append(_strip_comments(
                p.read_text(encoding='utf-8', errors='replace')))
        _AI_TEXT = '\n'.join(chunks)
    return _AI_TEXT


def _find_def_body(text, fragment):
    """在 text 里找 fragment 的**定义**并返回其函数体；找不到返回 ''。

    必须区分声明与定义：声明后面跟的是 ';'，定义后面跟的是 '{'。
    只按"片段之后第一个 '{'"找，会把声明也当成定义，于是抓到隔壁函数的
    函数体 —— 检查项就对着别人的代码说话了。
    """
    pos = 0
    while True:
        idx = text.find(fragment, pos)
        if idx < 0:
            return ''
        j = idx + len(fragment)
        depth = 0
        while j < len(text):
            c = text[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif depth == 0 and c == ';':
                break            # 只是声明/前向声明，继续找定义
            elif depth == 0 and c == '{':
                body, _ = _html_block(text, j)
                return body or ''
            j += 1
        pos = idx + len(fragment)


def _ai_func_body(fragment):
    """在 AI 协议分片里找函数的定义体；找不到返回 ''。

    调用方**必须**把"找不到"当失败处理 —— 否则会静默失去检查面，
    这正是 2026-11-22 那 119 条假警报的成因。
    """
    for p in _ai_source_paths():
        text = _strip_comments(p.read_text(encoding='utf-8', errors='replace'))
        body = _find_def_body(text, fragment)
        if body:
            return body
    return ''


def _src_func_body(fragment):
    """在 src/*.cpp 全体里找函数的定义体；找不到返回 ''。

    【为什么不能只查一个文件】命令面板的实现**分居三处**：
      · 取数（列出哪些命令）—— src/app_sequence_overlay.cpp 的 App::SequenceRows
      · 派发（怎么调、参数怎么收）—— src/app_commands.cpp 的 App::InvokeFromPalette
      · 连线（面板选中后走哪条）—— src/app_sequence_overlay.cpp 的 App::SequenceExecute
    判据锚在单个文件上，文件一拆分就整条失灵 —— 2026-11-22 那次第 15 项
    误报就是这么来的：判据还在 app_commands.cpp 的 ShowCommandPalette 里找
    `CommandTable()`，而那段代码早已搬去 app_sequence_overlay.cpp。

    与 _ai_func_body 同样的纪律：找不到返回 ''，由调用方判失败。
    """
    for p in sorted((ROOT / 'src').glob('*.cpp')):
        text = _strip_comments(p.read_text(encoding='utf-8', errors='replace'))
        body = _find_def_body(text, fragment)
        if body:
            return body
    return ''


def _ai_state_body():
    """StateToJson 的定义体（exbar/state 的**唯一**序列化来源）。

    第 17 项（可写状态回读）与第 18 项（断言左值求值）都锚在它身上 ——
    因为那两项要核对的是"state 里到底有没有这个字段"，
    而 exbar/state 的内容就是这函数的输出。
    """
    return _ai_func_body('std::wstring StateToJson')


def _fail_kind_codes():
    """FailKindCode 翻出来的错误码集合。

    【为什么要单独取】"被拦住"的四类码不在 ai_api 里写死 —— 那是**故意的**：
    类别由命令调用点声明，经 commands.h 的 FailKind 唯一映射成码。
    只扫 ai_api 就会漏掉它们，于是"分类退化成一锅端"这件事检查不出来 ——
    而那正是第 22 项要防的。

    找不到 FailKindCode 时返回空集：调用方据此判定"分类面没了"。
    """
    path = ROOT / 'src' / 'commands.cpp'
    if not path.exists():
        return set()
    text = _strip_comments(path.read_text(encoding='utf-8', errors='replace'))
    body = _find_def_body(text, 'const wchar_t* FailKindCode')
    if not body:
        return set()
    return set(re.findall(r'return L"([a-z_]+)"', body))


def _state_selection_observable():
    """观测面是否可读选中：StateToJson 是否输出 selectionCount。

    needsSelection 命令能否暴露给 AI 取决于此（见第 4 项与第 14 项）：
    观测面在场 → AI 可先读 selection 确认目标再调用；缺失 → 恢复"不许暴露"。

    返回三态，**不能**压成布尔：
      True  → 观测面在场
      False → 观测面在场，但确实没有输出 selectionCount（该收紧规则）
      None  → **定位失败**（找不到 StateToJson 的定义）——
               这不是"观测面缺失"，是检查面自己坏了，必须报错。
               压成 False 会把"我找不到"说成"它没有"，
               于是 2026-11-22 那 119 条假警报能一路静默走下去。
    """
    global _SEL_OBS
    if _SEL_OBS is None:
        body = _ai_state_body()
        if not body:
            _SEL_OBS = 'missing'
        else:
            _SEL_OBS = 'L"selectionCount"' in body
    return None if _SEL_OBS == 'missing' else _SEL_OBS


_SEL_OBS = None
_AI_TEXT = None


def _effects_constants(text):
    """收集 `constexpr Effects NAME{ro, de, idem, ow, undo, Scope::X};` 常量。

    【为什么需要】命令表里同一操作的两条入口（file.* 与 fs.*）必须引用
    **同一份** Effects 常量 —— 否则"同一事实两个源头"的问题会重新长出来
    （历史上就是这样：fs.delete 说可撤销、file.delete 说不可撤销，
    而暴露给 AI 的恰好是错的那份）。

    解析器必须能认出常量名，否则所有引用常量的命令会被当成
    "readOnly=0 / scope=App" 的默认值 —— 检查器就会对着假数据报错。
    """
    consts = {}
    pat = re.compile(
        r'constexpr\s+Effects\s+(\w+)\s*\{\s*([^}]*)\}\s*;')
    for m in pat.finditer(text):
        nums = [x.strip() for x in m.group(2).split(',') if x.strip()]
        if len(nums) < 6:
            continue
        sm = re.search(r'Scope::(\w+)', nums[5])
        consts[m.group(1)] = {
            'readOnly': nums[0] == '1', 'destructive': nums[1] == '1',
            'idempotent': nums[2] == '1', 'openWorld': nums[3] == '1',
            'undoable': nums[4] == '1', 'scope': sm.group(1) if sm else 'App',
        }
    return consts


def parse_commands():
    raw = COMMANDS.read_text(encoding='utf-8')
    text = _strip_comments(raw)
    consts = _string_constants(text)
    eff_consts = _effects_constants(text)

    # 表元数据常量（kNoArgs / kExempt* / kEffect*）与两段行表同放在
    # commands_table.h —— 读表必须连常量一起读：
    # argsSchema 写成常量名的那批条目，解析不到常量就会变成 None，
    # 进而报"schema 不是 JSON"这种指向错误的假错误。
    ctext = _strip_comments(TABLE_CONSTS_SRC.read_text(encoding='utf-8'))
    consts.update(_string_constants(ctext))
    eff_consts.update(_effects_constants(ctext))

    out = []
    table_text = []
    for p in TABLE_SRCS:
        ttext = _strip_comments(p.read_text(encoding='utf-8'))
        rows = _parse_table_rows(ttext, consts, eff_consts)
        if not rows:
            print('WARN 命令表段落解析为空：%s（形状变了？）' % p.name)
        out.extend(rows)
        table_text.append(ttext)
    # 返回表**本体**的文本（不是 commands.cpp）—— 调用方用它对账
    # "源码里写了多少条 CmdId:: 条目"，那必须对着表数，不能对着派发总线数。
    return out, '\n'.join(table_text), consts


def _parse_table_rows(text, consts, eff_consts):
    """从一段 `std::vector<CommandDef> XxxCommandRows() { return { ... }; }` 解出条目。

    命令表按作用域分在两个文件里（用户操作域 / 工具·Agent 域），
    两段各返回一张行表，由 commands_table.cpp 拼成整张。
    """
    m = re.search(r'std::vector<CommandDef>\s+\w+CommandRows\(\)\s*\{\s*return\s*\{', text)
    if not m:
        return []
    start = m.end() - 1
    inner, _ = _html_block(text, start)
    if inner is None:
        return []

    out = []
    i = 0
    while i < len(inner):
        c = inner[i]
        if c != '{':
            i += 1
            continue
        entry, end = _html_block(inner, i)
        if entry is None:
            break
        i = end
        fields = _split_top_level(entry)

        # 至少要有 id..needsSelection 十个字段，否则不是命令条目
        mm = re.match(r'CmdId::(\w+)', fields[0]) if fields else None
        if not mm:
            continue
        cid = mm.group(1)
        name = _wstring(fields[1]) if len(fields) > 1 else None
        group = _wstring(fields[2]) if len(fields) > 2 else None
        scopeui = _wstring(fields[3]) if len(fields) > 3 else None
        title = _wstring(fields[4]) if len(fields) > 4 else None
        impl = fields[5].strip() == 'true' if len(fields) > 5 else False
        eff = fields[6] if len(fields) > 6 else ''
        em = re.search(r'Effects\{(.*)\}', eff, re.S)
        nums = [x.strip() for x in em.group(1).split(',')] if em else []
        ro = de = idem = ow = undo = '0'
        scope = 'App'
        if len(nums) >= 6:
            ro, de, idem, ow, undo = nums[:5]
            sm = re.search(r'Scope::(\w+)', nums[5])
            scope = sm.group(1) if sm else 'App'
        # Effects 写成具名常量（kEffectDelete 之类）时按常量表解析。
        # 解析不到就当"全默认" —— 那样会让这条命令看起来像 readOnly=0/App，
        # 后面所有基于 Effects 的检查都会对着假数据说话。
        eff_const = eff.strip()
        if eff_const in eff_consts:
            e = eff_consts[eff_const]
            ro = '1' if e['readOnly'] else '0'
            de = '1' if e['destructive'] else '0'
            idem = '1' if e['idempotent'] else '0'
            ow = '1' if e['openWorld'] else '0'
            undo = '1' if e['undoable'] else '0'
            scope = e['scope']
        schema = _string_literal(fields[7], consts) if len(fields) > 7 else None
        exposed = fields[8].strip() == 'true' if len(fields) > 8 else False
        sel = fields[9].strip() == 'true' if len(fields) > 9 else False

        # 尾部可选字段按**位置**解析，不按"最后一个字符串"猜。
        #
        # struct 声明顺序固定为：requires_ / outputSchema / assertion / assertionExempt。
        # 关键：`nullptr` 是 outputSchema 的显式占位（"这条不产出结果"），
        # 它必须占掉一个位置 —— 否则后面的 assertion 会顶上来被当成结果 schema，
        # 报出"outputSchema 非法 JSON"这种指向错误的假错误。
        #
        # assertion 后面还有第四个槽 assertionExempt（显式豁免理由）：
        # 有豁免的条目形如 `..., nullptr, nullptr, "理由"` ——
        # 第二个 nullptr 是"这条没有断言"，第三个位置才是豁免理由。
        # 同样必须按位置读，否则豁免理由会被当成断言，
        # 报出"左值不在 state 里"这种指向错误的假错误。
        #
        # 例：
        #   ..., false, false, Requirements{}, "outputSchema", "assert"    → 12 字段
        #   ..., false, false, Requirements{}, nullptr,       "assert"     → 13 字段
        #   ..., false, false, Requirements{}, nullptr,       nullptr, "理由" → 14 字段
        reqs, outschema, assertion, exempt = [], None, None, None
        tail = [x.strip() for x in fields[10:]]
        tail = [x for x in tail if x]                      # 去掉空的尾逗号残余
        pos = 0
        if pos < len(tail) and tail[pos].startswith('Requirements'):
            rm = re.search(r'Requirements\{(.*)\}', tail[pos], re.S)
            if rm:
                reqs = [x.strip() for x in rm.group(1).split(',') if x.strip()]
            pos += 1
        if pos < len(tail) and tail[pos] == 'nullptr':
            pos += 1                                       # 占位，明确表示"无结果"
        else:
            if pos < len(tail):
                v = _string_literal(tail[pos], consts)
                if v is not None:
                    outschema = v
                    pos += 1
        if pos < len(tail) and tail[pos] == 'nullptr':
            pos += 1                                       # 占位，明确表示"无断言"
        else:
            if pos < len(tail):
                v = _string_literal(tail[pos], consts)
                if v is not None:
                    assertion = v
                    pos += 1
        if pos < len(tail):
            v = _string_literal(tail[pos], consts)
            if v is not None:
                exempt = v

        out.append({
            'id': cid, 'name': name, 'group': group, 'title': title,
            'scopeUi': scopeui, 'implemented': impl,
            'readOnly': ro == '1', 'destructive': de == '1',
            'idempotent': idem == '1', 'openWorld': ow == '1',
            'undoable': undo == '1', 'scope': scope,
            'schema': schema, 'exposed': exposed, 'needsSelection': sel,
            'requires': reqs, 'outputSchema': outschema, 'assertion': assertion,
            'assertionExempt': exempt,
            'effectsConst': eff_const if eff_const in eff_consts else None,
            'nfields': len(fields),
        })
    return out


def collect_arg_key_usage():
    """扫所有 handler 分片，收集每个命令 handler 里实际取用了哪些参数键。

    handler 写作 reg(CmdId::X, [..](const Args& args) { ... args.Int(L"preset") ... })
    所以按 reg(CmdId::X 切段，在段内抓 args.Int/Str/Bool("<键>")。
    """
    usage = {}
    for path in sorted((ROOT / 'src').glob(HANDLERS_GLOB)):
        text = _strip_comments(path.read_text(encoding='utf-8'))
        # reg( 与 regR( 都要扫：后者是"带返回值"的注册（第 5 步引入）。
        parts = re.split(r'\breg(?:R)?\(CmdId::(\w+)', text)
        # parts[0] 是前置内容，之后是 (id, body, id, body, ...)
        for i in range(1, len(parts) - 1, 2):
            cid = parts[i]
            body = parts[i + 1]
            # Has 不能单独算"取用"：只判断存在却不读值仍会静默丢弃参数。
            # 正常写法 Has(k) ? Int(k, default) : fallback 里的 Int 已能被下面抓到。
            # Any 是"原样取 JSON"的访问器（config.set 的 value 类型由数据决定，
            # 用 Int/Str/Bool 取会把错类型静默转默认值），算作正常取用。
            keys = set(re.findall(r'args\.(?:Int|Str|Bool|StrArray|Any)\(L"([^"]+)"', body))
            usage.setdefault(cid, set()).update(keys)
    return usage


def collect_indirect_arg_keys():
    """按 INDIRECT_ARGS 登记的公共实现逐个取函数体，收集它真正读过的参数键。

    这里刻意不取"所有 app_commands*.cpp 的并集"：那样任何一个命令读过 `targets`
    都会让另一个命令的 `targets` 被判为已使用，漏报"参数被丢弃"。
    """
    key_re = r'args\.(?:Int|Str|Bool|StrArray|Has)\(L"([^"]+)"'
    out = {}
    for name, helper in INDIRECT_ARGS.items():
        short = helper.rsplit('::', 1)[-1]
        keys = set()
        for path in sorted((ROOT / 'src').glob(HANDLERS_GLOB)):
            text = _strip_comments(path.read_text(encoding='utf-8'))
            pos = 0
            while True:
                idx = text.find(short, pos)
                if idx < 0:
                    break
                brace = text.find('{', idx)
                # 函数/lambda 定义：名字到左括号之间只有签名/捕获（<300 字符）。
                if 0 <= brace - idx < 300:
                    body, _ = _html_block(text, brace)
                    if body:
                        keys.update(re.findall(key_re, body))
                        break
                pos = idx + len(short)
        out[name] = keys
    return out


# ---------------------------------------------------------------- R20 支撑

def human_paths():
    """收集人类侧的命令入口：(命令名 -> [路径说明])。

    四条来源，都必须扫 —— 只扫一条会漏掉真实入口：
      · data/keymap.json 的热键（key 非空）与序列
      · src/keymap.cpp 的 kSysKeys（key 为空时回落到系统原生键）
      · src/input_router.cpp 的 kExtra（带内联参数的应用内补全）
      · src/*.cpp 里对 CommandBus 的直接调用（菜单/按钮/状态栏）
    """
    paths = {}

    def add(name, why):
        if not name:
            return
        paths.setdefault(name, [])
        if why not in paths[name]:
            paths[name].append(why)

    # 1) keymap.json
    if KEYMAP_JSON.exists():
        km = json.loads(KEYMAP_JSON.read_text(encoding='utf-8'))
        for h in km.get('hotkeys', []):
            spec = h.get('command', '')
            name, _, arg = spec.partition(':')
            if h.get('key'):
                add(name, f"热键 {h['key']}" + (f"（内联参数 {arg}）" if arg else ''))
        for s in km.get('sequences', []):
            spec = s.get('command', '')
            name, _, arg = spec.partition(':')
            add(name, f"序列 {s.get('seq')}" + (f"（内联参数 {arg}）" if arg else ''))

    # 2) kSysKeys：key 为空时回落到系统原生键（Ctrl+C / Delete …）
    if KEYMAP_CPP.exists():
        for m in re.finditer(r'\{L"(hotkey\w+)",\s*L"([^"]+)"\}',
                             KEYMAP_CPP.read_text(encoding='utf-8')):
            setting, combo = m.group(1), m.group(2)
            add(setting, f'系统原生键 {combo}')

    # 3) kExtra（input_router）：带内联参数
    if INPUT_ROUTER.exists():
        for m in re.finditer(r'\{L"([^"]+)",\s*L"(\w+)(?::([^"]*))?"\}',
                             INPUT_ROUTER.read_text(encoding='utf-8')):
            key, name, arg = m.group(1), m.group(2), m.group(3)
            add(name, f'应用内键 {key}' + (f'（内联参数 {arg}）' if arg else ''))

    # 4) 源码里的直接 Invoke（菜单 / 按钮 / 状态栏 / 双击）
    for p in sorted((ROOT / 'src').glob('*.cpp')):
        if p.name == 'commands.cpp' or p.name.startswith('ai_'):
            continue
        txt = _strip_comments(p.read_text(encoding='utf-8'))
        for m in re.finditer(r'Invoke\w*\(\s*CmdId::(\w+)\s*(?:,\s*([^;]*))?', txt):
            cid, argtext = m.group(1), (m.group(2) or '').strip()
            # 只判"有没有把参数递进去"：Args::Empty() 与空字面量都算没参数
            has_arg = bool(argtext) and not re.match(
                r'^(Args::Empty\(\)|Args\(\)|L""\s*$)', argtext)
            add(cid, f'{p.name} 直接调用' + ('（带参数）' if has_arg else '（无参数）'))
    return paths


def resolve_keymap_names():
    """把 hotkey 的 setting 名（hotkeyCmdCopy）映射回命令名。

    kSysKeys 的键是 setting，不是 command，所以要经 keymap.json 反查。
    """
    mapping = {}
    if KEYMAP_JSON.exists():
        km = json.loads(KEYMAP_JSON.read_text(encoding='utf-8'))
        for h in km.get('hotkeys', []):
            name, _, _ = h.get('command', '').partition(':')
            mapping[h.get('setting', '')] = name
    return mapping


def main():
    cmds, table_text, consts = parse_commands()
    if not cmds:
        fail('命令表解析为空 —— kTable 的形状变了？先修本脚本的结构解析')
        return 1

    # 硬断言：AI 协议分片必须能找到。
    # glob 写错 / 文件改名 → 后面每一项都对着空文本说话，
    # 照样打印"全部通过"。这与 2026-11-22 那次假警报同源，
    # 所以在这里硬拦一道。
    if not _ai_source_paths():
        fail(f'找不到任何 src/{AI_SRC_GLOB} —— AI 协议实现的定位失效，'
             f'本脚本后续结论全部不可信')
        return 1

    # 硬断言：解析结果必须覆盖表里所有条目。
    # 这一条是防"检查面静默缩水"的最后一道闸 —— 早先正则只命中 21/140
    # 却照样打印"全部通过"，就是因为没有这道对账。
    declared = len(re.findall(r'\{CmdId::', table_text))
    print(f'命令表：{len(cmds)} 条（源码中 CmdId:: 条目 {declared} 个）')
    if len(cmds) != declared:
        fail(f'解析条目数 {len(cmds)} != 源码条目数 {declared} —— '
             f'有条目没被解析到，检查面不完整，后续结论都不可信')
        return 1

    # schema 缺失 = 解析不到常量或字面量，属于解析层失败，单独报清楚
    noschema = [c['name'] for c in cmds if c['schema'] is None]
    if noschema:
        fail(f'{len(noschema)} 条的 argsSchema 解析不到（常量名写错？）：{noschema}')
        return 1

    exposed = [c for c in cmds if c['exposed']]
    implemented = [c for c in cmds if c['implemented']]
    print(f'其中 aiExposed：{len(exposed)} 条；已实现：{len(implemented)} 条\n')

    print('1. aiExposed 必须已实现')
    bad = 0
    for c in exposed:
        if not c['implemented']:
            fail(f'{c["name"]} 暴露给 AI 但 implemented=false')
            bad += 1
    if not bad:
        ok('全部已实现')

    print('\n2. argsSchema 必须是合法 JSON 对象')
    bad = 0
    schemas = {}
    for c in cmds:
        try:
            v = json.loads(c['schema'])
        except json.JSONDecodeError as e:
            fail(f'{c["name"]} 的 argsSchema 非法 JSON：{e}')
            bad += 1
            continue
        if not isinstance(v, dict):
            fail(f'{c["name"]} 的 argsSchema 不是 JSON 对象：{c["schema"][:60]}')
            bad += 1
            continue
        schemas[c['name']] = v
    if not bad:
        ok(f'{len(cmds)} 条全部合法（不只是暴露的那些）')

    print('\n3. schema 声明的参数必须被 handler 真正取用')
    usage = collect_arg_key_usage()
    indirect_usage = collect_indirect_arg_keys()
    by_name = {c['name']: c for c in exposed}
    bad = 0
    for name, schema in schemas.items():
        if name not in by_name:
            continue
        declared_keys = set((schema.get('properties') or {}).keys())
        if not declared_keys:
            continue
        if name in INDIRECT_ARGS:
            # 只读登记的那个公共实现的函数体，不再取所有 handler 的键并集 ——
            # 并集会让"A 命令读过 targets"替"B 命令背书，参数其实被 B 丢掉。
            used = indirect_usage.get(name, set())
        else:
            used = usage.get(by_name[name]['id'], set())
        unused = declared_keys - used
        if unused:
            fail(f'{name} 声明了参数 {sorted(unused)} 但 handler 没取用（参数会被丢弃）')
            bad += 1
    if not bad:
        ok('全部对齐')

    print('\n4. needsSelection 暴露的前提：观测面可读选中（先读 selection 再调用）')
    # 旧规则是"一律不暴露"——当时没有观测面，"AI 无法指定目标"是事实。
    # state 快照输出 selection 之后前提变了：AI 能先读 selection 确认目标
    # 再调用，这正是人机协作（用户在界面表达意图）。检测锚点是 ai_api.cpp
    # 里 StateToJson 对 selectionCount 的输出——观测面被删/断链立即恢复 ERROR。
    bad = 0
    sel_obs = _state_selection_observable()
    if sel_obs is None:
        fail('定位失败：找不到 StateToJson 的定义，观测面无从判定 —— '
             '这一项的结论不可信（"找不到"不等于"没有"，两者必须分开报）')
        bad += 1
    elif not sel_obs:
        for c in cmds:
            if c['needsSelection'] and c['exposed']:
                fail(f'{c["name"]} 依赖选中项却又暴露给 AI，且观测面不可读选中（目标不可表达）')
                bad += 1
        if not bad:
            ok('无 needsSelection 暴露（观测面缺失时该规则自动收紧）')
    else:
        ok(f'观测面在场（state 输出 selectionCount），'
           f'{sum(1 for c in cmds if c["needsSelection"] and c["exposed"])} 条'
           f' needsSelection 命令可暴露')

    print('\n5. 无参数命令的 schema 应显式声明不接受参数')
    bad = 0
    for name, schema in schemas.items():
        if schema.get('properties'):
            continue
        if schema.get('additionalProperties') is not False:
            fail(f'{name} 无参数但 schema 未写 additionalProperties:false'
                 f'（空 schema 语义是"接受任意参数"）')
            bad += 1
    if not bad:
        ok(f'全部显式（{sum(1 for s in schemas.values() if not s.get("properties"))} 条无参）')

    print('\n6. 效果四维自洽（第 2 步判据）')
    bad = 0
    # 6a. readOnly 与 destructive 互斥。注意 readOnly + openWorld 是合法的：
    #     "只读地访问进程外"（查文件属性、搜索、系统对话框里的只读页）不改状态，
    #     但确实跨越了进程边界。
    for c in cmds:
        if c['readOnly'] and c['destructive']:
            fail(f'{c["name"]} 标了 readOnly 又标 destructive（自相矛盾）')
            bad += 1
        # 6b. destructive 若只能作用于"当前选中"（scope=CurrentTab 且无目标参数），
        #     机器无法指定要破坏哪个对象 —— 这是设计债务，不是可以忽略的小问题。
        #     已登记在 KNOWN_NO_TARGET 的条目先放行，但必须显式登记（见下方说明）。
        if c['destructive'] and c['scope'] == 'CurrentTab':
            declared_keys = set((schemas[c['name']].get('properties') or {}).keys())
            if not declared_keys:
                if c['name'] in KNOWN_NO_TARGET:
                    continue
                fail(f'{c["name"]} 是 destructive + scope=CurrentTab 但没有目标参数'
                     f'（机器无法指定要破坏哪个对象）')
                bad += 1
        # 6c. undoable 与 destructive 可以共存：删除通常进回收站（可撤销），
        #     但网络盘/超配额/U 盘时会永久删除（不可逆）。destructive 的定义是
        #     "**可能**不可逆"，所以两者同时为真是对"删到回收站"最准确的描述。
        #     真正矛盾的是 destructive 却不提醒确认 —— 那个由 verify 侧保证（见下）。
    if not bad:
        ok('四维无矛盾')
    if KNOWN_NO_TARGET:
        print(f'  注意  {len(KNOWN_NO_TARGET)} 条旧命令仍只能作用于"当前选中"'
              f'（已有对应的 Fs* 确定式版本，见第 9 项）：')
        print(f'        {sorted(KNOWN_NO_TARGET)}')

    print('\n7. 特性声明覆盖度')    # "全否"并不总是错：非幂等、不破坏、不跨进程、不可撤销的普通写操作
    # （新建标签、切换标签）本来就没有任何一项为真。这里只统计覆盖度，
    # 让人看到"有多少条是这种朴素写操作"，而不是当成错误。
    plain = [c['name'] for c in cmds
             if c['implemented'] and not c['readOnly'] and not c['destructive']
             and not c['idempotent'] and not c['openWorld'] and not c['undoable']]
    described = len(cmds) - len(plain)
    ok(f'{described}/{len(cmds)} 条有明确特性；其余 {len(plain)} 条是'
       f'"非幂等、不破坏、不跨进程、不可撤销"的普通写操作')
    if plain:
        print(f'        例：{plain[:6]}{" ..." if len(plain) > 6 else ""}')

    print('\n8. 前置条件与 needsSelection 一致（第 3 步判据）')
    # needsSelection=true 的命令若暴露给 AI，必须有 HasSelection 前置 ——
    # 否则机器调用时会在"没有选中"的状态下执行，作用于空集合或错误对象。
    # 注意当前 needsSelection 的命令都不暴露（第 4 项已保证），所以这里
    # 检查的是"将来若放开暴露，前置条件必须先补齐"。
    bad = 0
    for c in cmds:
        if not c['implemented']:
            continue
        if c['needsSelection'] and 'HasSelection' not in ','.join(c['requires']):
            if c['exposed']:
                fail(f'{c["name"]} 暴露且依赖选中项，但没有 HasSelection 前置')
                bad += 1
    if not bad:
        ok('一致')

    # 统计有多少条带了前置条件
    withreq = [c['name'] for c in cmds if c['requires']]
    ok(f'{len(withreq)} 条声明了前置条件')

    print('\n9. 确定式命令（第 4 步判据：交互与确定分离）')
    # 约定：Fs* 前缀是"确定式"命令 —— 目标由参数点名、绝不弹框、绝不读选中。
    # 它们与旧的 Cmd*/CopyToFolder 等是同一个业务的两种入口。
    # 判据：
    #   a. 必须有目标参数（否则和旧命令没区别）
    #   b. 不能标 needsSelection（确定式不该依赖用户选中）
    #   c. 每条旧命令都应有对应的确定式版本（债务要被消化，不是永远挂着）
    fs = [c for c in cmds if c['name'].startswith('fs.')]
    bad = 0
    for c in fs:
        props = set((schemas[c['name']].get('properties') or {}).keys())
        # 本意是"目标由参数点名"。target/targets/path 都满足；
        # 只认前两个会误伤 FsOpen（打开一个路径，叫 path 比叫 target 更准）。
        if not (props & {'target', 'targets', 'path'}):
            fail(f'{c["name"]} 是确定式命令但没有 target/targets 参数')
            bad += 1
        if c['needsSelection']:
            fail(f'{c["name"]} 是确定式命令却标了 needsSelection（应靠参数定位）')
            bad += 1
    if not bad:
        ok(f'{len(fs)} 条确定式命令都有目标参数，且不依赖选中')

    # 债务消化进度：每个旧的"只能作用于选中"的命令，是否已有确定式对应
    # 配对表是模块级的 SAME_OPERATION_PAIRS（唯一登记处）——
    # 这里不再抄一份，两份各自演化必然分叉。
    COVERS = SAME_OPERATION_PAIRS
    names = {c['name'] for c in cmds}
    impl_names = {c['name'] for c in cmds if c['implemented']}
    # 注意：这里遍历 COVERS 全体，不筛 KNOWN_NO_TARGET。
    # 早先多加了 `if old in KNOWN_NO_TARGET` 这一层，结果 CmdRename/CmdDelete
    # 明明在 COVERS 里却从不被打印 —— 覆盖情况看起来比实际更差。
    for old, new in COVERS.items():
        if new in names and new in impl_names:
            ok(f'{old} -> {new}（已覆盖）')
        else:
            fail(f'{old} 的确定式版本 {new} 缺失或未实现')
    # BatchRename / PathCopyLink 暂无确定式版本：显式列出，不假装完成。
    # CmdCopy 也写了 FsCopy，但它俩都是"写剪贴板"——需要单独说明为何仍算覆盖。
    uncovered = [n for n in sorted(KNOWN_NO_TARGET | set(COVERS)) if n not in COVERS]
    if uncovered:
        print(f'  待做  {uncovered} 尚无确定式版本')

    print('\n10. 返回值声明与实现一致（第 5 步判据）')
    # outputSchema 声明了"我会返回这个形状"，就必须由 RegisterWithResult 注册 ——
    # 否则调用方按 schema 去取字段会全落空，且没有任何报错。
    #
    # 判据有三条，缺一不可：
    #   a. 有 outputSchema 的必须用 regR/RegisterWithResult 注册；
    #   b. outputSchema 本身必须是合法 JSON 对象（否则客户端校验必失败）；
    #   c. **未实现的命令不得声明 outputSchema** —— 它根本不会执行，
    #      schema 是兑现不了的承诺（search.find 就踩过：implemented 降级成 false
    #      时只翻了 implemented 一个字段，outputSchema 还留着，
    #      于是"恒失败"与"有结构化结果"同时成立）。
    src_text = ''
    for path in sorted((ROOT / 'src').glob(HANDLERS_GLOB)):
        src_text += _strip_comments(path.read_text(encoding='utf-8'))
    result_regs = set(re.findall(r'(?:regR|RegisterWithResult)\(CmdId::(\w+)', src_text))

    bad = 0
    with_output = []
    for c in cmds:
        if not c['outputSchema']:
            continue
        with_output.append(c['name'])
        if not c['implemented']:
            fail(f'{c["name"]} 声明了 outputSchema 但 implemented=false'
                 f'（命令不会执行，结果承诺兑现不了）')
            bad += 1
        if c['id'] not in result_regs:
            fail(f'{c["name"]} 声明了 outputSchema 但没有用 RegisterWithResult 注册'
                 f'（结果永远是空的）')
            bad += 1
        try:
            ov = json.loads(c['outputSchema'])
            if not isinstance(ov, dict):
                fail(f'{c["name"]} 的 outputSchema 不是 JSON 对象')
                bad += 1
        except json.JSONDecodeError as e:
            fail(f'{c["name"]} 的 outputSchema 非法 JSON：{e}')
            bad += 1
    if not bad and with_output:
        ok(f'{len(with_output)} 条带 outputSchema 的命令都已正确注册且 schema 合法：'
           f'{with_output}')
    elif not with_output:
        print('  注意  尚无命令声明 outputSchema')

    print('\n11. 命名空间别名（第 6 步判据）')
    naming = ROOT / 'src/cmd_naming.cpp'
    aliases = []
    if naming.exists():
        txt = naming.read_text(encoding='utf-8')
        aliases = re.findall(r'\{L"(\w+)",\s*L"([a-z0-9.]+)"\}', txt)

    if not aliases:
        fail('cmd_naming.cpp 里没有解析到别名')
    else:
        # 命令名从解析结果取，不依赖行内位置 ——
        # 否则尾部一坏，别名检查会连带误报"源命令不存在"，把真正的
        # 问题（字段漏写）埋在假错误里。
        legacy_names = {c['name'] for c in cmds}
        dotted = {}
        bad = 0
        # 11a. 每个别名都要指向真实命令
        #
        # 迁移期（docs/NAMING.md §4.3 规则 C2）：某条命令的 d.name 从旧名改成点分名后，
        # 别名表里那一行 `{旧名, 点分名}` 就与 d.name 重合，源命令自然不在 legacy_names
        # 里了。这不是错误 —— 恰恰是"旧名保留、主名迁走"的中间态。判据改成：
        # 源名存在于命令表 **或** 目标点分名就是该命令的现用主名（即自映射）。
        for legacy, dot in aliases:
            if legacy not in legacy_names and dot not in legacy_names:
                fail(f'别名 {legacy} -> {dot}：源命令不存在')
                bad += 1
            # 11b. 点分名只能有小写字母/数字/点，且至少两段
            if not re.fullmatch(r'[a-z0-9]+(\.[a-z0-9]+)+', dot):
                fail(f'{legacy} 的点分名 {dot} 不合规（应是小写字母数字点、至少两段）')
                bad += 1
            # 11c. 点分名唯一
            if dot in dotted:
                fail(f'点分名 {dot} 重复（{dotted[dot]} 与 {legacy}）')
                bad += 1
            dotted[dot] = legacy
        if not bad:
            ok(f'{len(aliases)} 条别名：源命令存在、格式合规、无重复')

        # 11d. 覆盖率
        #
        # 覆盖要按**两个方向**统计：表里已迁到点分名的命令（win.quit），它的
        # 别名行是 `{QuitExplorer, win.quit}` —— 左列是旧名、右列才是现用主名。
        # 只按左列算会把它误报成"尚无显式别名"。
        covered = {a[0] for a in aliases} | {a[1] for a in aliases}
        missing = legacy_names - covered
        if missing:
            print(f'  注意  {len(missing)}/{len(legacy_names)} 条命令尚无显式别名'
                  f'（会退回推导名）：{sorted(missing)[:6]}'
                  f'{" ..." if len(missing) > 6 else ""}')
        else:
            ok(f'全部 {len(legacy_names)} 条命令都有显式别名')

    print('\n12. MCP 工具形状（第 7 步判据）')
    # MCP 规范：tools/list 的每一项都必须有 name / description / inputSchema，
    # 且 inputSchema 必须是 JSON 对象（不能是字符串或 null）。
    # 这些在运行时只会表现为"客户端静默丢弃这个工具"，很难查。
    #
    # 锚点是 ToolsToJson 的**定义体**（不写死文件名）：序列化代码搬到哪个
    # 分片都不该让检查项失效 —— 写死文件名造成的 119 条假警报就是这么来的。
    tools_body = _ai_func_body('std::wstring ToolsToJson')
    if not tools_body:
        fail('找不到 ToolsToJson 的定义 —— MCP 工具形状这一项没有检查面了'
             '（函数被改名或搬走时，这里必须立刻红，而不是静默通过）')
        return 1
    required_fields = ['name', 'legacyName', 'title', 'description',
                       'annotations', 'inputSchema']
    missing_fields = [f for f in required_fields if f'o.Set(L"{f}"' not in tools_body]
    if missing_fields:
        fail(f'ToolsToJson 缺少字段：{missing_fields}')
    else:
        ok(f'ToolsToJson 输出 {len(required_fields)} 个必需字段')

    # inputSchema / outputSchema 必须是解析后的对象，不能塞字符串
    if ('json::Parse(t.argsSchema, schema' in tools_body
            and 'o.Set(L"inputSchema", schema)' in tools_body):
        ok('inputSchema 解析成对象（不是字符串）')
    else:
        fail('inputSchema 没有解析成对象 —— MCP 要求它是 JSON 对象')

    # annotations 四要素
    ann_fields = ['readOnlyHint', 'destructiveHint', 'idempotentHint', 'openWorldHint']
    miss_ann = [f for f in ann_fields if f'ann.Set(L"{f}"' not in tools_body]
    if miss_ann:
        fail(f'annotations 缺少：{miss_ann}')
    else:
        ok('annotations 含四个标准 hint')

    print('\n13. 条目字段完整性（防"漏写字段被默认值静默填上"）')
    # 聚合初始化允许省略尾部字段，编译器不报错、用默认值填。
    # 我加 outputSchema 时就漏掉了 aiExposed/needsSelection 两个布尔，
    # 结果 OpListDir/OpStat 声明了 outputSchema 却永远进不了 tools/list ——
    # 编译通过、测试也没覆盖到，只有手工数才知道。
    #
    # 判据：每个条目必须显式写出 id..needsSelection 这 10 个字段，
    # 不能靠默认值。现在有结构解析，按字段个数判，比正则可靠。
    bad = [c['name'] for c in cmds if c['nfields'] < 10]
    if bad:
        fail(f'{len(bad)} 条字段不足 10 个（尾部靠默认值静默填）：{bad}')
    else:
        ok(f'{len(cmds)} 条全部显式声明 aiExposed/needsSelection')

    print('\n14. 暴露决策的合理性')
    # aiExposed=true 意味着机器能直接驱动这条命令。判据：
    #   a. 未实现的不能暴露（会在 tools/list 里出现但调不动）
    #   b. 依赖选中项的**不该**暴露 —— 机器无法指定"选哪个"，
    #      暴露出去就是"AI 以为删了 A，实际删了 B"
    #   c. 纯人机 UI（弹菜单/开关面板/退出程序）暴露没有意义
    #
    # 例外名单：确有理由的逐条登记，让"为什么它在这儿"可见。
    UI_ONLY = {
        'nav.viewmenu',     # 弹视图菜单，机器调了得到的是一个没人看的菜单
        'win.quit',         # 关程序不该由机器替人决定（旧名 QuitExplorer，见 NAMING.md §4.5）
        'win.folderoptions',  # 弹系统对话框
        'win.mapdrive',     # 弹系统对话框
        'win.unmapdrive',   # 弹系统对话框
        'win.palette',      # 调色板是给人键入命令用的
        'win.hotkeys',      # 快捷键配置面板
    }
    bad = 0
    for c in cmds:
        if not c['exposed']:
            continue
        if not c['implemented']:
            fail(f'{c["name"]} 暴露给 AI 但未实现（会出现却调不动）')
            bad += 1
        # 依赖选中项：观测面在场（第 4 项检测过）才允许暴露——AI 先读
        # selection 确认目标再调用；观测面缺失或**定位失败**时这里恢复 ERROR。
        if c['needsSelection'] and sel_obs is not True:
            fail(f'{c["name"]} 暴露给 AI 但依赖当前选中且观测面不可读（机器无法指定目标）')
            bad += 1
        if c['name'] in UI_ONLY:
            fail(f'{c["name"]} 是纯人机 UI，不该暴露')
            bad += 1
    if not bad:
        ok(f'{len(exposed)} 条暴露的命令都合理（已实现、不依赖选中、非纯 UI）')

    unexp_impl = [c for c in cmds if c['implemented'] and not c['exposed']]
    if unexp_impl:
        print(f'  说明  {len(unexp_impl)} 条已实现但未暴露：')
        for c in unexp_impl:
            why = '依赖选中' if c['needsSelection'] else ('纯人机 UI' if c['name'] in UI_ONLY else '待决')
            print(f'        [{why}] {c["name"]}')

    print('\n15. 人机等价（R20：每个 aiExposed 命令都必须有人工可达路径）')
    # R20 约束的是**人能不能做到同一件事**，不是"命令 id 在人类侧有没有调用点"。
    # 判定分两条路（见文件头 HUMAN_PATH 的注释）：
    #
    #   A. 无必填参数 → 命令面板（Ctrl+Shift+P）列出全部已实现命令并以
    #      Args::Empty() 调用。这是机制保证，只要 implemented=true 就成立。
    #      所以这一类的"可达"不需要逐条找调用点 —— 早先我按调用点扫，
    #      结果把 nav.home / session.save / tab.close.duplicates 这类全报成
    #      不可达，其实人在面板里点得到、点了也能成。那是扫描器的错，不是代码的错。
    #
    #   B. 有必填参数 → 面板那条路点了必然失败（拿不到参数）。必须另有一条
    #      能把参数递进去的路：热键内联参数、带参的源码调用，或 HUMAN_PATH 登记。
    #
    # 另有 AI_ONLY_QUERIES：人能触发、但结果只在审计日志里（结构化数据给 agent 用）。
    # 它们不算违规，但要显式登记，让"人点了看不到东西"是已知事实。
    paths = human_paths()
    setting_to_cmd = resolve_keymap_names()

    # kSysKeys 登记的是 setting 名，转成命令名才有用
    for setting, cmd in setting_to_cmd.items():
        if setting in paths:
            for why in list(paths[setting]):
                paths.setdefault(cmd, []).append(why)
            del paths[setting]

    # 命令面板覆盖哪些命令：**实现分居三处，判据必须跨三处核对**。
    #
    # 判据变过两次，原因值得记下来：
    #   旧判据 = "筛掉未实现与有必填参数的命令，其余空参调用"。
    #   它防的是真危险（tab.rename/tab.color 空参调用会静默清别名/颜色），
    #   但代价是把 config.get / config.set / file.rename 这类带参命令
    #   在人类侧整条抹掉 —— 只有 AI 能触发，正好违反本项要守护的 R20。
    #   而且"面板跳过"根本不是防静默清空的唯一办法：按 schema 追问参数再调用，
    #   同样不会空参调用，还多了人类入口。
    #
    #   再旧一版判据 = 只在 app_commands.cpp 的 ShowCommandPalette 一个函数体里
    #   找四个特征（CommandTable() / !d.implemented / Args::Empty() /
    #   Args::FromJson(）。**判据指向的位置已经过时** —— 面板取数搬到了
    #   app_sequence_overlay.cpp 的 App::SequenceRows，派发搬到了
    #   app_commands.cpp 的 App::InvokeFromPalette，两者由
    #   app_sequence_overlay.cpp 的 App::SequenceExecute 连起来。
    #   于是判据对着空文本说话，2026-11-22 报出一条"命令面板没覆盖全"的假警报，
    #   而实际功能是好的（44/44 深测全绿）。
    #
    # 现在的判据（任一不成立都说明"带参命令又在人类侧漏掉了"）：
    #   ① 取数处（App::SequenceRows）：遍历整张命令表且只收 implemented 的 ——
    #      带必填参数的命令也必须在列，不能被"有参"筛掉。
    #   ② 派发处（App::InvokeFromPalette）：无必填参数走 Args::Empty()，
    #      有必填参数走 Args::FromJson(收集结果) —— 两条路都在。
    #   ③ 收参处（CollectParamsInteractive）：从 argsSchema 里读
    #      required/properties —— 不允许硬编码某几条命令的参数名，
    #      否则新增命令又得改面板（回到打补丁）。
    #   ④ 连线处（App::SequenceExecute）：面板选中后必须真的走进派发处，
    #      否则"面板列得出来但点了不执行参数收集"照样是缺口。
    #      这一条是 ① ② 之间的桥 —— 少了它，两处各自成立也可能是两条平行线。
    rows_body = _src_func_body('App::SequenceRows')
    if not rows_body:
        fail('找不到 App::SequenceRows 的定义 —— 命令面板的取数处判据失效，'
             '本项后续结论不可信')
        return 1
    # implemented 的过滤写法有两种（取反 / 取正），两种都算"只收已实现的"。
    list_all = ('CommandTable()' in rows_body
                and ('d.implemented' in rows_body or '!d.implemented' in rows_body))
    if not list_all:
        fail('命令面板必须"列出所有已实现命令"（遍历整张命令表 + 按 implemented 过滤）'
             '—— 否则带参命令在人类侧不可达（R20）')
        return 1

    dispatch_body = _src_func_body('App::InvokeFromPalette')
    if not dispatch_body:
        fail('找不到 App::InvokeFromPalette 的定义 —— 命令面板的派发处判据失效，'
             '本项后续结论不可信')
        return 1
    dispatch_ok = ('Args::Empty()' in dispatch_body
                   and 'Args::FromJson(' in dispatch_body)
    if not dispatch_ok:
        fail('命令面板必须"列出所有已实现命令"，且无必填参数走空参、'
             '有必填参数走按 schema 收集后的参数 —— 否则带参命令在人类侧不可达（R20）')
        return 1

    collect_body = _src_func_body('bool CollectParamsInteractive')
    if not collect_body:
        fail('找不到 CollectParamsInteractive 的定义 —— 参数收集处判据失效，'
             '本项后续结论不可信')
        return 1
    # 判据查的是"**从 schema 里读**"，不是"字符串出现过"。
    # 只查 '"required"' 这种字面量的话，把 schema.Find(L"required") 改成
    # 读一个常量、或者在函数里留一句注释提 required，都能蒙混过关 ——
    # 那样判据守的就不是"参数真的来自 schema"这件事了。
    if not ('Find(L"required")' in collect_body
            and 'Find(L"properties")' in collect_body):
        fail('命令面板的参数收集器必须从 argsSchema 读 required/properties'
             '（schema.Find(L"required") / schema.Find(L"properties")），'
             '不能硬编码某几条命令的参数名')
        return 1

    # 连线：面板选中 → 派发（收集参数就发生在派发里）。
    exec_body = _src_func_body('App::SequenceExecute')
    if not exec_body:
        fail('找不到 App::SequenceExecute 的定义 —— 面板与派发之间的连线判据失效，'
             '本项后续结论不可信')
        return 1
    if 'InvokeFromPalette' not in exec_body:
        fail('命令面板选中命令后必须走 InvokeFromPalette（统一派发口），'
             '否则带参命令拿不到参数 —— 违反 R20')
        return 1

    palette_all = True

    def required_of(c):
        return sorted((schemas[c['name']].get('required') or []))

    def param_source(c):
        """B 路：返回一条能把必填参数递进去的人类路径说明，找不到返回 None。"""
        # 命令面板已按 argsSchema 追问必填参数（见上面 palette_all 的两条判据），
        # 所以"面板覆盖"本身就是一条能把参数递进去的路，不必每条命令再登记一次。
        if palette_all and not c.get('destructive'):
            return '命令面板：按 argsSchema 追问必填参数后调用'
        own = (paths.get(c['name']) or []) + (paths.get(c['id']) or [])
        for w in own:
            if '带参数' in w or '内联参数' in w:
                return w
        return HUMAN_PATH.get(c['name'])

    missing_path, ai_only = [], []
    for c in exposed:
        req = required_of(c)
        if c['name'] in AI_ONLY_QUERIES:
            ai_only.append(c['name'])
            continue
        if not req:
            continue          # A 路：面板覆盖，机制保证
        if not param_source(c):
            missing_path.append((c['name'], req))

    if missing_path:
        for name, req in missing_path:
            fail(f'{name} 有必填参数 {req} 但人类侧没有任何路径能把参数递进去'
                 f'—— 违反 R20：不得存在只有 AI 能触发的命令')
    else:
        ok(f'{len(exposed)} 条暴露命令全部有人工可达路径'
           f'（无参 {sum(1 for c in exposed if not required_of(c) and c["name"] not in AI_ONLY_QUERIES)} 条走命令面板，'
           f'有参 {sum(1 for c in exposed if required_of(c) and c["name"] not in AI_ONLY_QUERIES)} 条有参数来源）')

    if ai_only:
        # 藏起来的例外等于没有规则：逐条打印，让人能复核。
        print(f'  说明  {len(ai_only)} 条人能触发但结果只在审计日志里（结构化数据给 agent）：')
        for name in sorted(ai_only):
            print(f'        {name}：{AI_ONLY_QUERIES[name]}')

    if HUMAN_PATH:
        # 登记项要能对得上真实命令 —— 名字写错时它不会报错，只会静默失效，
        # 让"已经登记过了"变成一句空话。同理，登记一条早已不暴露的命令
        # 会让人以为问题解决了。
        exposed_names = {c['name'] for c in exposed}
        stale = [n for n in HUMAN_PATH if n not in exposed_names]
        if stale:
            fail(f'HUMAN_PATH 里 {len(stale)} 条不在 aiExposed 集合内（登记失效）：{sorted(stale)}')
        stale_ai = [n for n in AI_ONLY_QUERIES if n not in exposed_names]
        if stale_ai:
            fail(f'AI_ONLY_QUERIES 里 {len(stale_ai)} 条不在 aiExposed 集合内：{sorted(stale_ai)}')

        shown = [n for n in sorted(HUMAN_PATH) if n in exposed_names]
        if shown:
            print(f'  说明  {len(shown)} 条有参命令的参数来自界面（逐条可复核）：')
            for name in shown:
                print(f'        {name}：{HUMAN_PATH[name]}')

    # ---------------------------------------------------------------- 16
    print('\n16. UI 菜单动作是否有结构化入口（反向覆盖）')
    # 前 15 项查的都是 **AI → 人**（暴露的命令必须人能调）。这一项查反方向：
    # **人 → 结构化**。用户右键菜单里点得到的动作，如果命令表里没有对应项，
    # 那它就只能是"UI 里的一段私有代码" —— 快捷键绑不上、命令面板搜不到、
    # 外部脚本与 AI 都够不着，且没有任何检查会报出来。
    #
    # 这条缺口真实存在过：书签的删除/编辑/新建分类/撤销/清空五项只写在
    # bookmark_bar.cpp 的右键菜单 switch 里，BookmarkStore 的 Remove/AddFolder/
    # ReplaceAll/Undo 除了 UI 之外零调用点。
    #
    # 登记表 MENU_ACTIONS：'人工入口' → 覆盖它的命令别名（None = 已知缺口）。
    # 覆盖后必须真的存在于命令表，否则登记失效（防"改个名就当覆盖了"）。
    # 用**全部**命令的别名（不只是 aiExposed）：菜单动作的对应命令不必暴露给
    # AI（例如 path.copy 依赖选中），但必须存在，否则快捷键/面板也够不到。
    alias_all = {c['name'] for c in cmds}
    covered = 0
    missing = []
    for human, alias in MENU_ACTIONS.items():
        if human in MENU_DISABLED_HINTS:
            # 禁用提示行不是动作：点它什么也不发生，也就没有"结构化入口"可言。
            # 之前把它们登记成 None，结果与真缺口混在同一张"待做"清单里 ——
            # 一份写着"1 项待补命令"的清单，点开发现是"（暂无搜索历史）"，
            # 真缺口反而被这行噪声冲淡了。现在单列一类，不参与覆盖计数。
            continue
        if alias is None:
            missing.append(human)
        elif alias not in alias_all:
            fail(f'MENU_ACTIONS 登记 {human} → {alias}，但命令表里没有这个别名')
        else:
            covered += 1
    if missing:
        print(f'  待做  {len(missing)} 项人工可达但无结构化入口（已登记，尚未补命令）：')
        for h in missing:
            print(f'        {h}')
    ok(f'{covered} 项菜单动作已有结构化入口')
    if MENU_DISABLED_HINTS:
        print(f'  ok     {len(MENU_DISABLED_HINTS)} 项是禁用提示行（不是动作，不计入覆盖）：'
              f'{sorted(MENU_DISABLED_HINTS)}')

    # 16b. 覆盖率：扫菜单源码里**实际存在**的文案，凡是没登记的都必须报出来。
    #
    # 只靠上面那张手工表时，新增一个菜单项却忘了登记，检查会静默通过 ——
    # 那正是「菜单里写了就完事」再次长出来的方式。这里反查源码：
    # 菜单文案是字符串字面量，抽得出来；抽得到就必须在表里有交代。
    # 【2026-11-22 补盲】旧版正则只认 kXxxMenuYyy 符号动作号，且只扫两个文件。
    # 书签栏/状态栏/面包屑/导入弹层全用数字动作号（1/2/900/3000），
    # 一个都扫不到 —— 于是"全部通过"只覆盖了标签右键。现在：
    #   · 正则放宽到任意动作号（符号或数字），只要求文案有交代；
    #   · 扫描面扩到全部含 ui::Menu 链的文件。
    #
    # 【2026-11-23 再补盲：文件名不能写死】上表原来是七条硬编码路径，
    # 而视图菜单当天被拆到新文件 src/nav_bar_view_menu.cpp（职责拆分，行为不变）——
    # 于是 5 条视图菜单项立刻报"表已腐烂"，而它们其实一行没改。
    # 判据指向**文件路径**的时候，重构一次就失效一次；改成"扫描所有出现
    # ui::Menu 的源文件"，把区域名当纯展示标签（只用于拼登记名，不再决定扫不扫）。
    #
    # AddPath/AddChecked 用变量文案（如 kSegNames[i]、sortHint），
    # 抽不出静态文案，不进入本项——它们的覆盖由第 15 项与命令表保证。
    AREA_OF_FILE = {
        'tab_strip_actions.cpp': '标签',
        'bookmark_bar.cpp': '书签',
        # 书签栏弹出层已从 bookmark_bar.cpp 拆出（bookmark_bar_popup.cpp =
        # 四套旧弹层；bookmark_category_popup.cpp = 可拖动的分类内容弹层）。
        # 区域名沿用"书签"，登记名因此保持不变。
        'bookmark_bar_popup.cpp': '书签',
        'bookmark_category_popup.cpp': '书签',
        'nav_bar_actions.cpp': '视图',
        'nav_bar_view_menu.cpp': '视图',
        'app_view_menus.cpp': '视图',
        'app_commands.cpp': '状态栏',
        'bookmark_io.cpp': '书签导入',
        'app_commands_files.cpp': '详情列',
        'app_search.cpp': '搜索',
        'nav_pane_shell.cpp': '导航',
        # 设置面板的两处弹层：枚举档位下拉（options 来自键表，文案是变量）与
        # 行高档位。都是"改一个值"的菜单，与 config.set 同一族。
        'settings_dialog_actions.cpp': '设置',
    }
    # 判据是"**真的在这里构建菜单**"，不是"文件里出现过 ui::Menu 这个词"：
    # app_state.cpp 只是调 ui::Menu::SetShortcutResolver（把快捷键文案接给组件），
    # 它一行菜单都不建 —— 按字符串判会把它误报成"漏登记区域"。
    # 构造菜单的判据是出现 `ui::Menu <名字>;` 或 `ui::Menu ... .Add(` 链。
    MENU_BUILD_RE = re.compile(r'ui::Menu\s+\w+\s*;')
    scan_sources = []
    for p in sorted((ROOT / 'src').glob('*.cpp')):
        text = p.read_text(encoding='utf-8', errors='replace')
        if not MENU_BUILD_RE.search(text):
            continue
        area = AREA_OF_FILE.get(p.name)
        if area is None:
            fail(f'{p.name} 构建了 ui::Menu 但没在 AREA_OF_FILE 里登记区域名 —— '
                 '不登记就等于这个文件的菜单项逃出第 16 项（漏登记会静默通过）')
            continue
        scan_sources.append((area, str(p.relative_to(ROOT)).replace('\\', '/')))
    if not scan_sources:
        fail('找不到任何构建 ui::Menu 的源文件 —— 第 16 项的扫描面塌了，本项结论不可信')
    # 「完成」是弹层关闭行、「最近关闭」是禁用标题行，都不是独立操作。
    MENU_SCAN_IGNORE = {'完成', '最近关闭'}
    registered = set(MENU_ACTIONS.keys())
    unregistered = []
    all_menu_src = []
    for area, rel in scan_sources:
        src = (ROOT / rel).read_text(encoding='utf-8', errors='replace')
        all_menu_src.append(src)
        for m in re.finditer(
                r'\.Add(?:Checked)?\(\s*L"([^"]+)"\s*,\s*([^,)\s]+)', src):
            label = m.group(1).replace('...', '').strip()
            if label in MENU_SCAN_IGNORE:
                continue
            # 锁定项文案随状态变（锁定/解锁），归一到同一登记名。
            if '锁定标签' in label or '解锁标签' in label:
                label = '锁定'
            full = area + '·' + label
            if full in registered:
                continue
            unregistered.append((full, m.group(2)))
    if unregistered:
        for lab, act in unregistered:
            fail('菜单项「%s」(%s) 没在 MENU_ACTIONS 登记：要么它有结构化入口（登记别名），'
                 '要么是已知缺口（登记 None）' % (lab, act))
    else:
        print('  ok     源码里的菜单项全部已在 MENU_ACTIONS 有交代')

    # 16c. 登记表反向校验：表里的每项都必须在扫描源码里真有对应文案。
    # 这条防的是"表腐烂"——改名/删菜单后登记项留着，
    # 检查照样绿，但那张表已经不描述现实了。
    stale = []
    all_text = '\n'.join(all_menu_src)
    for human in MENU_ACTIONS:
        _, _, label = human.partition('·')
        probe = '锁定标签' if label == '锁定' else label
        if f'L"{probe}' not in all_text and f'L"{label}' not in all_text:
            stale.append(human)
    if stale:
        for h in stale:
            fail(f'MENU_ACTIONS 登记「{h}」在扫描源码里找不到对应文案 —— 表已腐烂')
    else:
        print(f'  ok     {len(MENU_ACTIONS)} 项登记全部能在源码找到对应文案')


    # ---------------------------------------------------------------- 17
    print('\n17. 可写状态必须有回读（写完能验证）')
    # AI 能**改**的界面状态，必须能从 exbar/state 读回来。只有写没有读时，
    # `isError=false` 就成了"我改成功了"的唯一证据 —— 而它其实可能表示
    # "前置条件跳过""参数取了默认值""被用户刚做的操作覆盖"。
    # 这类缺陷不会当场报错，只会让调用方基于错误认知继续下一步。
    #
    # 判据是机械的：命令表里凡是改这几个维度（排序/分组/书签栏/窗格）的命令，
    # StateSnapshot 就必须有对应字段、且 StateToJson 真的输出它。
    ai_h = (ROOT / 'src' / 'ai_api.h').read_text(encoding='utf-8', errors='replace')
    # exbar/state 的内容就是 StateToJson 的输出，所以"能不能回读"这件事
    # 要在**它的定义体**里查，而不是在某个文件里查（分片搬走就失效的教训）。
    state_body = _ai_state_body()
    if not state_body:
        fail('找不到 StateToJson 的定义 —— 可写状态回读这一项没有检查面了')
        return 1
    readback_missing = []
    for field, cmds_writing in WRITABLE_STATE:
        in_struct = re.search(rf'\b{field}\b', ai_h) is not None
        in_json = re.search(rf'L"{field}"', state_body) is not None
        if not (in_struct and in_json):
            readback_missing.append((field, cmds_writing,
                                     '结构体缺字段' if not in_struct else 'StateToJson 未输出'))
    if readback_missing:
        for field, cmds_writing, why in readback_missing:
            fail(f'{cmds_writing} 可改该状态，但 exbar/state 无法回读 {field}（{why}）')
    else:
        ok(f'{len(WRITABLE_STATE)} 个可写状态字段都能回读')

    # ---------------------------------------------------------------- 18
    print('\n18. 自断言必须可求值（防假绿）')
    # 断言的左值必须取自 exbar/state 的真实字段；$arg: 引用的键必须在
    # argsSchema 里声明。否则这条断言要么永远为真、要么永远为假 ——
    # 两种都是假绿，比没有断言更危险（它让人以为验证过了）。
    state_fields = set()
    for m2 in re.finditer(r'v\.Set\(L"(\w+)"', state_body):
        state_fields.add(m2.group(1))
    bad_assert = []
    checked_assert = 0
    for c in cmds:
        a = c.get('assertion')
        if not a:
            continue
        checked_assert += 1
        # 运算符有两种：= 与 !=。以前这里只按第一个 '=' 切，
        # 于是 "selectionCount!=0" 被切成 lhs="selectionCount!" ——
        # 报"左值不在 state 里"，指向的是一个根本不存在的字段。
        # 断言语言早就支持 !=，检查却没跟上，等于**禁止使用 !=**。
        m_op = re.search(r'(!=|=)', a)
        if not m_op:
            bad_assert.append((c['name'], a, '缺少 = 或 !='))
            continue
        lhs = a[:m_op.start()].strip()
        rhs = a[m_op.end():].strip()
        if not lhs:
            bad_assert.append((c['name'], a, '左值为空'))
            continue
        # 左值可以是 "<字段>" 或 "<字段>.length"（取数组/字符串长度）。
        lhs_field = lhs[:-len('.length')] if lhs.endswith('.length') else lhs
        if lhs_field not in state_fields:
            bad_assert.append((c['name'], a, '左值 %s 不在 exbar/state 里' % lhs_field))
        # 右值三种形态：字面量 / $arg:<key>[.length|.trim] / $changed / $unchanged
        if rhs in ('$changed', '$unchanged'):
            continue
        if rhs.startswith('$arg:'):
            key = rhs[5:]
            for suffix in ('.length', '.trim'):
                if key.endswith(suffix):
                    key = key[:-len(suffix)]
            schema_txt = c.get('schema') or ''
            needed = '"' + key + '"'
            if needed not in schema_txt:
                bad_assert.append((c['name'], a, '引用的入参 %s 未在 argsSchema 声明' % key))
    if bad_assert:
        for name, a, why in bad_assert:
            fail('%s 的断言「%s」不可求值：%s' % (name, a, why))
    else:
        ok('%d 条命令带自断言，且左值与入参引用都可求值' % checked_assert)

    # ---------------------------------------------------------------- 19
    print('\n19. 事件流必须可订阅、可续接（不能只给轮询）')
    # 判据钉三件事，少任何一件都会退化成"客户端只能猜"：
    #   · 有序号 + 历史补发（断线重连才知道从哪接）
    #   · 有真正保持连接的推送（不是换个名字的轮询）
    #   · 状态变化由服务端检测后推送（客户端不必自己定时拉 state）
    ev_h = (ROOT / 'src/ai_events.h').read_text(encoding='utf-8', errors='replace')
    ev_cpp = (ROOT / 'src/ai_events.cpp').read_text(encoding='utf-8', errors='replace')
    srv = ai_server_source()
    # API 侧的分片（ai_api*.cpp）合并后查 —— 命令结果事件由"执行"分片发，
    # 工具/能力面的形状由"序列化"分片发，两边都要在检查面里。
    api = _ai_all_text()
    missing = []
    for label, cond in [
        ('事件序号/补发接口', all(k in ev_h for k in ('Publish', 'Since', 'WaitFor', 'LastSeq'))),
        ('等待有通知机制（condition_variable）', 'condition_variable' in ev_cpp),
        ('历史缓冲有上限', 'kMaxEvents' in ev_cpp),
        ('推送行带 event 标记与 seq', 'L"event"' in ev_cpp and 'L"seq"' in ev_cpp),
        ('列出历史 events/since', srv.count('L"events/since"') >= 2),
        ('订阅 events/subscribe', srv.count('L"events/subscribe"') >= 2),
        ('订阅模式真的在推事件', 'events::WaitFor' in srv and 'subscribed' in srv),
        ('状态变化由 PumpRequests 检测', 'events::PollStateChanges()' in srv),
        ('命令结果发事件（含被拒绝的）', 'events::Publish(L"command"' in api
         and 'CallImpl(' in api),
        ('有事件流单测', (ROOT / 'tests/events_test.cpp').exists()),
    ]:
        if not cond: missing.append(label)
    if missing:
        for label in missing:
            fail('事件流缺：' + label)
    else:
        ok('事件可订阅/可补历史/带序号，状态变化由服务端推送')

    # ---------------------------------------------------------------- 20
    print('\n20. 多实例必须有独立 profile（测试不污染用户数据）')
    main_cpp = (ROOT / 'src/main.cpp').read_text(encoding='utf-8', errors='replace')
    cfg_h = (ROOT / 'src/config.h').read_text(encoding='utf-8', errors='replace')
    cfg_cpp = (ROOT / 'src/config.cpp').read_text(encoding='utf-8', errors='replace')
    ident = (ROOT / 'src/identity.h').read_text(encoding='utf-8', errors='replace')
    profile_missing = []
    for label, cond in [
        ('环境变量常量', 'kProfileEnv' in ident),
        ('Config 提供目录切换', 'void SetProfileDir' in cfg_h and 'void Config::SetProfileDir' in cfg_cpp),
        ('切目录重置内存配置', 'loaded_ = false' in cfg_cpp and 's_ = Settings{}' in cfg_cpp),
        ('MULTI 与 PROFILE 都参与判定', 'kMultiInstanceEnv' in main_cpp and 'kProfileEnv' in main_cpp),
        ('显式 profile 真的传给 Config', 'SetProfileDir(profileBuf)' in main_cpp),
        ('不再有单实例互斥（多实例允许）', 'CreateMutexW' not in main_cpp and
         'kInstanceMutex' not in ident),
        ('显式多开走 --new-window', 'kNewWindowArg' in ident and
         'NewWindowRequested' in main_cpp),
        ('目录切换早于配置被读取', main_cpp.find('SetProfileDir(profileBuf)') <
         main_cpp.find('App::Get().Run(')),
        ('路径全部从 Dir() 派生', all(f'Dir()' in cfg_cpp for _ in range(1)) and
         'SettingsPath() const { return tf::Join(Dir()' in cfg_cpp and
         'BookmarksPath() const { return tf::Join(Dir()' in cfg_cpp),
        ('有隔离单测', (ROOT / 'tests/profile_isolation_test.cpp').exists()),
    ]:
        if not cond: profile_missing.append(label)
    if profile_missing:
        for label in profile_missing:
            fail('测试隔离缺：' + label)
    else:
        ok('MULTI/PROFILE 实例落在独立目录，派生路径与内存配置都随之切换')

    # ---------------------------------------------------------------- 21
    print('\n21. 开发面必须形成可验证的闭环（快照/编译/测试/回滚）')
    table = '\n'.join(p.read_text(encoding='utf-8', errors='replace')
                      for p in TABLE_SRCS)
    dev_cpp = (ROOT / 'src/dev_tools.cpp').read_text(encoding='utf-8', errors='replace')
    dev_handlers = (ROOT / 'src/app_commands_dev.cpp').read_text(encoding='utf-8', errors='replace')
    # 破坏性命令的人工确认收口在 **CommandBus::Dispatch**（2026-11-22 从
    # InvokeFromPalette 下沉），判据在 src/commands_confirm.cpp，弹框由
    # App 注入的回调执行（src/app_confirm.cpp）——
    # 判据跟着搬，否则它守的是一个已经不管事的调用点。
    dispatch_body = _src_func_body('bool CommandBus::Dispatch')
    # 判据层：NeedsConfirm 的实现 + 豁免表在 commands_confirm.cpp。
    # 豁免表在匿名 namespace 里，不在 NeedsConfirm 的函数体内 —— 要读整个文件。
    confirm_rule_file = ROOT / 'src' / 'commands_confirm.cpp'
    confirm_rule_text = ''
    confirm_rule_body = ''
    if confirm_rule_file.exists():
        confirm_rule_text = confirm_rule_file.read_text(encoding='utf-8', errors='replace')
        confirm_rule_body = _find_def_body(confirm_rule_text, 'bool CommandBus::NeedsConfirm')
    # 弹框层：App 侧注入的回调。回调体是 lambda，没有具名函数可锚 ——
    # 按 SetConfirmFn 调用点找它后面第一个 '{'（那就是 lambda 体）。
    confirm_fn_body = ''
    for p in sorted((ROOT / 'src').glob('*.cpp')):
        text = p.read_text(encoding='utf-8', errors='replace')
        idx = text.find('SetConfirmFn')
        if idx < 0:
            continue
        brace = text.find('{', idx)
        if brace < 0:
            continue
        body, _ = _html_block(text, brace)
        if body and 'MB_YESNO' in body:
            confirm_fn_body = body
            break
    dev_missing = []
    # P0-9：dev.diff 与 changedFiles 的 mtime/sha256 是"改写可见性"的一对判据 ——
    # 缺任何一个，agent 改源码那一步就又回到信任边界之外。
    diff_cpp = (ROOT / 'src/dev_tools.cpp').read_text(encoding='utf-8', errors='replace')
    # 判据绑"行为/结构"，不绑函数名与文件位置：Diff 引擎一度在 dev_tools.cpp
    # （std::wstring Diff()/CurrentChanges()），后被抽到独立的 dev_diff.cpp
    # （devdiff::Report Compute）——实现更完整，但写死旧符号的判据会假失败。
    diff_engine = (ROOT / 'src/dev_diff.cpp').read_text(encoding='utf-8', errors='replace')
    diff_engine_h = (ROOT / 'src/dev_diff.h').read_text(encoding='utf-8', errors='replace')
    # 解析层已拆到独立文件（src/dev_diff_parse.cpp）——判据绑"行为"不绑"文件位置"，
    # 所以两个文件的内容合起来看。
    diff_parse = (ROOT / 'src/dev_diff_parse.cpp').read_text(encoding='utf-8', errors='replace')
    diff_all = diff_engine + diff_parse
    snap_cpp = (ROOT / 'src/dev_snapshot.cpp').read_text(encoding='utf-8', errors='replace')
    snap_h = (ROOT / 'src/dev_snapshot.h').read_text(encoding='utf-8', errors='replace')
    for label, cond in [
        ('dev.diff 命令已注册', 'L"dev.diff"' in table),
        ('dev.diff 有带结果的处理器', 'regR(CmdId::DevDiff' in dev_handlers),
        ('dev.diff 只读不改工作树', 'Effects{1, 0, 1, 1, 0' in table),
        ('diff 引擎独立且只读（Compute 返回结构化 Report）',
         'Report Compute' in diff_engine and 'devdiff::Compute' in diff_cpp),
        ('changedFiles 带 mtime 与 sha256',
         'std::wstring mtime' in snap_h and 'std::wstring sha256' in snap_h
         and 'f.mtime = ' in snap_cpp and 'f.sha256 = ' in snap_cpp
         and 'std::wstring mtime' in diff_engine_h and 'std::wstring sha256' in diff_engine_h),
        ('diff 输出含 sha256/mtime/status/hunks 字段',
         'L"sha256"' in diff_cpp and 'L"mtime"' in diff_cpp
         and 'L"status"' in diff_cpp and 'L"hunks"' in diff_cpp),
        # P0-9 重写后的四条行为判据 —— 少任何一条，"agent 改了什么"就又漏一块：
        #   ① 未跟踪的新文件：git diff 的所有变体都看不见它，必须单独取，
        #      否则 agent 写出来的新文件一个字都看不到（还报 added=0，像"没改"）。
        #   ② 清单与内容同源：清单（name-status）、行数（numstat）、内容（patch）
        #      都得出自 diff HEAD。混用 git status（索引口径）会让重命名判定对不上。
        #   ③ 截断必须上报：输出被 256 KB 上限截断后看起来和完整的一样 ——
        #      不报出来就是假绿（审阅者以为看全了）。
        #   ④ 纯重命名时 git 不发 ---/+++ 行，要从 rename to/from 取路径，
        #      否则那一项会被整个丢掉。
        ('未跟踪的新文件单独取（git diff 看不见它）',
         '"--others"' in diff_engine and '"--exclude-standard"' in diff_engine),
        ('清单与内容同源（相对 HEAD，不用索引口径）',
         '"--name-status"' in diff_engine and '"--numstat"' in diff_engine),
        ('输出截断必须上报（不静默截断）',
         'patchOverflow' in diff_engine and 'nameStatusOverflow' in diff_engine),
        ('纯重命名也能取到路径（git 不发 ---/+++ 行）',
         'rename to ' in diff_all and 'rename from ' in diff_all),
        ('diff 有内容级单测（在真仓库里造场景）',
         (ROOT / 'tests/dev_diff_test.cpp').exists()),
    ]:
        if not cond: dev_missing.append(label)
    for label, cond in [
        ('六条开发命令齐全', all(('L"dev.' + n + '"') in table for n in
                              ('snapshot', 'diff', 'build', 'test', 'job', 'rollback'))),
        ('耗时作业异步且发完成事件', 'std::thread' in dev_cpp and 'dev.job.done' in dev_cpp),
        ('全部用带结果的注册', dev_handlers.count('regR(CmdId::Dev') >= 5),
        # 破坏性命令人侧要确认：**三层都在**才算数 ——
        #   ① 判据层（NeedsConfirm + 豁免表）：谁要问；
        #   ② 收口层（Dispatch 里调 ConfirmIfDestructive）：所有人工入口统一过这一道门；
        #   ③ 弹框层（App 注入的回调真的弹框）：问得到、问得准。
        # 只查一处的漏法：门在但没弹框（点了直接执行）、弹了框但没设门
        # （弹框成了摆设）、或者判据层压根没有（调用点拼错名字，照样"通过"）。
        ('破坏性命令人侧要确认',
         bool(dispatch_body) and 'ConfirmIfDestructive' in dispatch_body
         and bool(confirm_rule_body) and 'kNoConfirmDestructive' in confirm_rule_text
         and bool(confirm_fn_body) and 'MB_YESNO' in confirm_fn_body),
        ('回滚有路径校验', 'ValidRepoPath' in dev_cpp and '..' in dev_cpp),
        ('有开发面单测', (ROOT / 'tests/dev_tools_test.cpp').exists()),
    ]:
        if not cond: dev_missing.append(label)
    if dev_missing:
        for label in dev_missing:
            fail('开发面闭环缺：' + label)
    else:
        ok('快照/编译/测试/作业/回滚齐全，异步结果可订阅，回滚有人侧确认')

    # ---------------------------------------------------------------- 21b
    # 豁免表**内容**的交叉核对（不是"表在不在"，是"表里装的是什么"）。
    #
    # 【为什么单独查】豁免表是破坏性命令唯一的免问通道 ——
    # 表里多一条，就多一条命令在人工侧不弹框。所以"往表里加一条"是最容易
    # 也最不容易被发现的拆门方式：判据只看"豁免表存在"的话，加多少条都照过。
    #
    # 两条判据：
    #   ① 表里每一条都必须是**真被标成 destructive** 的命令 ——
    #      混进非 destructive 的条目，说明这张表正在变成"什么都豁免"。
    #   ② 表的规模有上限 —— 每加一条都是一次"少问一次"的决定，
    #      应当被审视，而不是悄悄变长。
    print('\n21b. 破坏性命令的"日常豁免"表不许被悄悄放宽')
    cmd_by_id = {c['id']: c for c in cmds}
    exempt_ids = []
    m = re.search(r'kNoConfirmDestructive\[\]\s*=\s*\{(.*?)\}', confirm_rule_text, re.S)
    if m:
        exempt_ids = re.findall(r'CmdId::(\w+)', m.group(1))
    if not exempt_ids:
        fail('读不到破坏性命令的豁免表（kNoConfirmDestructive）—— '
             '判据层失配，无法核对豁免范围')
    else:
        not_destructive = [cid for cid in exempt_ids
                           if cid in cmd_by_id and not cmd_by_id[cid]['destructive']]
        if not_destructive:
            fail(f'豁免表里混进了非破坏性命令：{not_destructive} —— '
                 '豁免表的每一条都必须是"被标成 destructive 但实际可恢复/无害"的命令')
        else:
            ok(f'豁免表 {len(exempt_ids)} 条，每一条都是真破坏性命令')
        if len(exempt_ids) > 15:
            fail(f'豁免表有 {len(exempt_ids)} 条（上限 15）—— '
                 '每加一条都是一次"少问一次"的决定，应先复核是否真的无害')
        else:
            ok(f'豁免表规模 {len(exempt_ids)} 条（上限 15）')

    # ---------------------------------------------------------------- 22
    print('\n22. 失败必须可机器分类、能力面必须自描述（不靠客户端猜）')
    api_h_text = (ROOT / 'src/ai_api.h').read_text(encoding='utf-8', errors='replace')
    api_cpp_text = _ai_all_text()
    srv_text = ai_server_source()
    test_text = (ROOT / 'tests/ai_api_test.cpp').read_text(encoding='utf-8', errors='replace')

    # 错误码是客户端的分支依据：多一个未登记的码 = 客户端会掉进 default 分支，
    # 少一个码 = 客户端只能解析中文（一改文案就断）。所以两端都钉住。
    #
    # 【为什么多了一处取码的地方】"被拦住"的四类码不在 ai_api 里写死，
    # 而是由 commands.h 的 FailKind 经 FailKindCode 翻出来（唯一真源）。
    # 只扫 ai_api 会漏掉它们 —— 那正是本项要防的事：分类退化回一锅端。
    known_codes = {'unknown_tool', 'not_exposed', 'not_implemented', 'bad_args',
                   'schema_invalid', 'danger_disabled', 'need_approval',
                   'precondition_unmet', 'lock_conflict', 'canceled_by_user',
                   'not_supported', 'timed_out',
                   'failed', 'effect_not_met'}
    used_codes = set(re.findall(r'errorCode = L"([a-z_]+)"', api_cpp_text))
    used_codes |= _fail_kind_codes()
    cap_body = _ai_func_body('std::wstring CapabilitiesToJson')
    if not cap_body:
        fail('找不到 CapabilitiesToJson 的定义 —— 能力面这一项没有检查面了')
        return 1

    gap_missing = []
    for label, cond in [
        ('结果结构有 errorCode', 'std::wstring errorCode' in api_h_text),
        ('能力面接口已声明', 'CapabilitiesToJson' in api_h_text),
        ('"调不到"三态可区分',
         {'unknown_tool', 'not_exposed', 'not_implemented'} <= used_codes),
        ('参数/审批/阻断各有独立码',
         {'bad_args', 'danger_disabled', 'need_approval',
          'precondition_unmet', 'lock_conflict', 'canceled_by_user',
          'not_supported'} <= used_codes),
        ('"被拦住"没有退化回一锅端', 'blocked' not in used_codes),
        ('"效果不符"有独立码', 'effect_not_met' in used_codes),
        ('码值都在白名单内（无拼写漂移）', used_codes <= known_codes),
        ('响应与事件都带 errorCode', api_cpp_text.count('L"errorCode"') >= 2),
        ('旧格式暴露 capabilities', 'L"capabilities"' in srv_text),
        ('JSON-RPC 暴露 exbar/capabilities', 'L"exbar/capabilities"' in srv_text),
        ('能力面直接数命令表（不另存清单）', 'CommandTable()' in cap_body),
        ('两类缺口清单都给出',
         'notImplemented' in cap_body and 'notExposed' in cap_body),
        ('有单测守着 errorCode 与能力面',
         'errorCode' in test_text and 'CapabilitiesToJson' in test_text),
    ]:
        if not cond: gap_missing.append(label)
    if gap_missing:
        for label in gap_missing:
            fail('缺口面缺：' + label)
    else:
        ok('失败按 %d 类机器码返回（含"被拦住"的四类 + 超时）；'
           '能力面（总数/已实现/已暴露/两张缺口清单）由产品自述'
           % len(used_codes))

    # ---------------------------------------------------------------- 23
    print('\n23. 自断言覆盖率门槛（每条要么有断言，要么有**带理由**的豁免）')
    # 为什么要有门槛：
    #   `ok=true` 只说明 handler 返回了 true，不说明"效果对吗"。
    #   断言是唯一能把这两件事分开的机制 —— 而覆盖率上不来时，
    #   没人能看出"哪些命令还没写断言"，因为没写和写不出来长得一模一样。
    #
    # 门槛的形式不是"必须 100% 有断言"（那不现实：磁盘、剪贴板、进程外
    # 根本没有回读面），而是"**必须有交代**"：
    #   要么给一条真能求值的断言，要么给一条显式豁免 + 为什么。
    # 于是覆盖率上不来的部分变成一份有名有姓的债务清单，而不是一片沉默。
    #
    # 豁免理由还有两条硬要求：
    #   · 不能太短（"不知道""以后再说"这类等于没写）；
    #   · 必须对调用方可见（tools/list 里给出 assertionExempt）——
    #     否则调用方会把 ok=true 当成"效果确实达成"。
    target = [c for c in cmds if c['exposed'] and c['implemented']]
    with_a = [c for c in target if c.get('assertion')]
    with_e = [c for c in target if c.get('assertionExempt')]
    both = [c for c in target if c.get('assertion') and c.get('assertionExempt')]
    neither = [c for c in target if not c.get('assertion') and not c.get('assertionExempt')]
    cover_missing = []
    for c in target:
        ex = c.get('assertionExempt')
        if ex is not None and len(ex.strip()) < 8:
            cover_missing.append((c['name'], '豁免理由太短（少于 8 字）：' + ex))
    # 豁免理由必须真的被暴露给调用方（否则这份清单只有开发者看得见）
    api_exposes_exempt = 'L"assertionExempt"' in api_cpp_text
    if not api_exposes_exempt:
        cover_missing.append(('（全表）', 'tools/list 未输出 assertionExempt：豁免对调用方不可见'))
    for c in both:
        cover_missing.append((c['name'], '同时有断言与豁免（自相矛盾）'))
    for c in neither:
        cover_missing.append((c['name'], '既无断言也无豁免'))
    if cover_missing:
        for name, why in cover_missing:
            fail('%s：%s' % (name, why))
    else:
        pct = (100.0 * len(with_a) / len(target)) if target else 0
        ok('%d 条暴露命令：%d 条有自断言（%.0f%%）、%d 条显式豁免（理由非空且已暴露给调用方）'
           % (len(target), len(with_a), pct, len(with_e)))

    # ---------------------------------------------------------------- 24
    print('\n24. 同义命令配对的 Effects 必须一致（契约单一来源）')
    # 为什么要有这一项：
    #   `file.*` 与 `fs.*` 做的是同一件事、走同一批实现，却各抄了一份 Effects。
    #   于是同一个事实有两个源头，而**暴露给 AI 的那份恰好是更乐观的那份**
    #   （fs.delete 说可撤销、fs.move 说不破坏性）。AI 依据这些声明决定
    #   "敢不敢试错" —— 声明错了，它就是基于错的认知行动。
    #
    # 判据是两条：
    #   ① 配对的两条命令，六个 Effects 字段必须逐项相同；
    #   ② 它们必须引用**同一个**具名常量（kEffectXxx）——
    #      只满足 ① 的话，下一个人加一条命令时又会分叉（回到老问题）。
    by_name = {c['name']: c for c in cmds}
    pair_missing = []
    pair_drift = []
    for a, b in sorted(SAME_OPERATION_PAIRS.items()):
        ca, cb = by_name.get(a), by_name.get(b)
        if ca is None or cb is None:
            pair_missing.append('%s / %s：配对里有命令不在表里' % (a, b))
            continue
        fields = ('readOnly', 'destructive', 'idempotent', 'openWorld',
                  'undoable', 'scope')
        diff = [f for f in fields if ca[f] != cb[f]]
        if diff:
            pair_drift.append(
                '%s 与 %s 的 Effects 不一致（%s）—— 同一件事不能有两套声明'
                % (a, b, '、'.join(diff)))
        elif ca.get('effectsConst') is None or cb.get('effectsConst') is None:
            pair_drift.append(
                '%s 与 %s 必须引用同一个具名 Effects 常量'
                '（当前 %s / %s）—— 否则"同一事实两个源头"会重新长出来'
                % (a, b, ca.get('effectsConst'), cb.get('effectsConst')))
        elif ca['effectsConst'] != cb['effectsConst']:
            pair_drift.append(
                '%s 与 %s 引用了不同常量（%s / %s）'
                % (a, b, ca['effectsConst'], cb['effectsConst']))
    for m in pair_missing + pair_drift:
        fail(m)
    if not pair_missing and not pair_drift:
        ok('%d 组同义命令共用同一份 Effects 常量，六个字段逐项一致'
           % len(SAME_OPERATION_PAIRS))

    # ---------------------------------------------------------------- 25
    print('\n25. 可逆性不许说谎（声明 undoable 的命令必须有真实撤销机制）')
    # 为什么要有这一项：
    #   `undoable=1` 是**给 AI 看的承诺** —— 它据此决定"敢不敢试错"。
    #   空承诺比不声明更坏：不声明 AI 会谨慎，声明了它反而会放心地做破坏性尝试。
    #
    # 2026-11-21 实测：全表 24 条声明 undoable，只有 13 条 bmk.* 真的有栈，
    # 其余 11 条是空承诺（tab.move.* 没有顺序快照、tab.pin 没有取消历史、
    # file.undo 本身不是被撤的对象、session.named.* 没有版本栈）。
    # 本轮把它们改成 0，并给真正可撤的 fs.* / file.rename.batch 补了
    # 自己的撤销栈（src/file_undo.h）。
    #
    # 判据两条（必须同时成立）：
    #   ① 声明 undoable=1 的命令必须登记在 UNDO_MECHANISM 里（事实清单）；
    #   ② 登记在册的命令必须真的声明 undoable=1（两处口径不许漂）。
    by_name2 = {c['name']: c for c in cmds}
    bad = 0
    claimed = []
    for c in cmds:
        if not c.get('undoable'):
            continue
        claimed.append(c['name'])
        if c['name'] not in UNDO_MECHANISM:
            fail('%s 声明 undoable=1 但没有真实撤销机制'
                 '（登记在 UNDO_MECHANISM 里才有栈）' % c['name'])
            bad += 1
    for name in sorted(UNDO_MECHANISM):
        c = by_name2.get(name)
        if c is None:
            fail('UNDO_MECHANISM 里的 %s 不在命令表里（登记失效）' % name)
            bad += 1
        elif not c.get('undoable'):
            fail('%s 登记了撤销机制但没有声明 undoable=1（两处口径必须一致）' % name)
            bad += 1
    if not bad:
        ok('%d 条声明可撤销的命令都指向真实撤销机制；%d 条操作无撤销栈已如实标 0'
           % (len(claimed), len(cmds) - len(claimed)))

    # ---------------------------------------------------------------- 26
    # 第 2 档 序 7（C2b + C1 + C2 + C3）：接受器是 archive 的前置。
    # 没有它，"分数涨了就留"会让 archive 越滚越脏 —— PACE 实测贪心接受
    # 30–42% 虚假提交、10–33% 有害。这里钉的是**机制**，不是中文注释：
    #   ① 配对比较而非单次总分：输入是成对的 candidate/baseline；
    #   ② 符号下注 e-process，因子 1+λX，阈值 1/α —— 不是 if (score>base)；
    #   ③ 不可用配对 fail-closed（不许把超时当平局丢掉）；
    #   ④ archive 存源码 + 分数 + **原始**轨迹（不预摘要，哈希可核对）；
    #   ⑤ funnel 五级齐全，评估面改动进不了 archive；
    #   ⑥ 蒙特卡洛检验 H0 假接受率 ≤ α —— 没这条，"接受器"可能只是假绿。
    print('\n26. 自我迭代必须有真接受器（配对 + e-process，不许分数涨了就留）')
    arc_h = (ROOT / 'src/dev_archive.h').read_text(encoding='utf-8', errors='replace')
    arc_eval = (ROOT / 'src/dev_archive_eval.cpp').read_text(encoding='utf-8', errors='replace')
    arc_cpp = (ROOT / 'src/dev_archive.cpp').read_text(encoding='utf-8', errors='replace')
    arc_handlers = (ROOT / 'src/app_commands_archive.cpp').read_text(encoding='utf-8', errors='replace')
    arc_test = (ROOT / 'tests/dev_archive_test.cpp').read_text(encoding='utf-8', errors='replace')
    arc_missing = []
    for label, cond in [
        ('三条 archive 命令齐全',
         all(('L"dev.archive.' + n + '"') in table for n in ('decide', 'record', 'list'))),
        ('接受器用常量 α/λ，调用方不能运行时调小',
         'constexpr double kAlpha' in arc_h and 'constexpr double kLambda' in arc_h),
        ('配对试验带 candidate 与 baseline 两侧',
         'double candidate' in arc_h and 'double baseline' in arc_h),
        ('e-process 用符号下注因子 1+λX（不是单次分数比较）',
         '1.0 + kLambda' in arc_eval and 'r.eProcess *=' in arc_eval),
        ('阈值是 1/α 而非自报分数',
         'r.eProcess >= r.threshold' in arc_eval and 'kThreshold = 20.0' in arc_h),
        ('不可用配对 fail-closed', 'r.invalid > 0' in arc_eval and '拒绝接受' in arc_eval),
        ('archive 逐字保存原始轨迹并记 SHA-256',
         'trajectory/raw.txt' in arc_cpp and 'Sha256Of' in arc_cpp and '不摘要' in arc_cpp),
        ('funnel 五级齐全（parse/prescan/compile/checks/stable）',
         all(('L"' + st + '"') in arc_eval
             for st in ('parse', 'prescan', 'compile', 'checks', 'stable'))),
        ('评估面改动进不了 archive',
         'protectedChanged.empty()' in arc_eval and 'evalsurface::Check()' in arc_handlers),
        ('有蒙特卡洛检验 H0 假接受率 ≤ α',
         'H0 假接受率' in arc_test and 'rate <= devarchive::kAlpha' in arc_test),
        ('有 archive 落盘往返单测',
         'ReadTrajectory' in arc_test and 'cand-test' in arc_test),
    ]:
        if not cond: arc_missing.append(label)
    if arc_missing:
        for label in arc_missing: fail('自我迭代接受器缺：' + label)
    else:
        ok('配对 e-process（E ≥ 1/α）+ 原始轨迹 archive + 五级 funnel，且有假接受率实测')

    # ---------------------------------------------------------------- 27
    # 第 2 档 序 8（B8 + B5 + P2-9 + B12）：事件必须跨重启可追溯。
    # 只留内存环形缓冲 = 反思没有原料；没有 trace/turn/step = 无法把一条
    # 事件对回"第几轮第几步"；不读 _meta.traceparent = 跨进程 trace 断开。
    print('\n27. 事件必须落盘 + 三级轨迹 + 请求 _meta 贯穿')
    ev2_h = (ROOT / 'src/ai_events.h').read_text(encoding='utf-8', errors='replace')
    ev2_cpp = (ROOT / 'src/ai_events.cpp').read_text(encoding='utf-8', errors='replace')
    ev2_srv = ai_server_source()
    ev2_test = (ROOT / 'tests/events_test.cpp').read_text(encoding='utf-8', errors='replace')
    ev2_missing = []
    for label, cond in [
        ('事件落盘 events.jsonl 并按大小轮转',
         'L"events.jsonl"' in ev2_cpp and 'tf::AppendLine(EventsPath()' in ev2_cpp
         and 'kRotateBytes' in ev2_cpp),
        ('启动时恢复序号与历史（不是重启就清零）',
         'void LoadFromDisk()' in ev2_cpp and 'events::LoadFromDisk();' in ev2_srv),
        ('Clear 只清内存，磁盘历史留着',
         'void Clear()' in ev2_cpp and 'g_events.clear();' in ev2_cpp),
        ('事件带 traceId / spanId / turn / step / ts',
         all(k in ev2_h for k in ('std::wstring traceId', 'std::wstring spanId',
                                  'int turn', 'int step', 'std::wstring ts'))),
        ('落盘与推送同源投影（字段不会只对一边有）',
         'json::Value ToValue(const Event& e)' in ev2_cpp
         and 'json::Value v = ToValue(e)' in ev2_cpp),
        ('W3C traceparent 严格解析（55 字符 / ff 版本 / 全零 id）',
         'text.size() != 55' in ev2_cpp and '0xff' in ev2_cpp
         and 'traceZero' in ev2_cpp and 'spanZero' in ev2_cpp),
        ('请求读 params._meta.traceparent',
         'TraceContextFromRequest' in ev2_h and 'L"traceparent"' in ev2_cpp
         and 'L"_meta"' in ev2_cpp),
        ('请求边界建 scope，审计 traceId 与事件同源',
         'RequestScope traceScope(req)' in ev2_srv
         and 'events::CurrentTraceIdOr(' in ev2_srv),
        ('嵌套请求分派不另开一轮', 'if (t_armed) return;' in ev2_cpp),
        ('单测覆盖 traceparent / 三级轨迹 / 落盘往返',
         'ParseTraceparent(tp' in ev2_test and 'LoadFromDisk()' in ev2_test
         and 'GetStr(L"parentSpanId")' in ev2_test),
        ('落盘事件不入库（不污染 dev.diff）',
         'data/events.jsonl' in (ROOT / '.gitignore').read_text(encoding='utf-8', errors='replace')),
    ]:
        if not cond:
            ev2_missing.append(label)
    if ev2_missing:
        for label in ev2_missing:
            fail('事件落盘 / 三级轨迹缺：' + label)
    else:
        ok('事件落盘可跨重启续接，traceId/turn/step 贯穿请求与审计')

    # ---------------------------------------------------------------- 28
    # 第 2 档 序 9（C5 + C5b + B7）：经验沉淀不能变成作弊传播通道。
    # EVOMAL 实测恶意技能自发复制 4.9–9.0 倍、删源后第 5 轮仍有 68% 攻击
    # 成功率；技能池超临界规模反而拉低表现。所以这里钉的是四道硬闸：
    # 先验证再入库 / 可疑隔离 / 容量上限 + 淘汰 / 每次变更进 ledger。
    print('\n28. 跨会话经验必须先验证再入库，且带污染防护与统计')
    exp_py = ROOT / 'tools/experience.py'
    exp_text = exp_py.read_text(encoding='utf-8', errors='replace') if exp_py.exists() else ''
    ctl_text = (ROOT / 'tools/agentctl.py').read_text(encoding='utf-8', errors='replace')
    exp_missing = []
    for label, cond in [
        ('独立经验模块存在', exp_py.exists()),
        ('ExpeL 四动作 ADD/UPVOTE/DOWNVOTE/EDIT 齐全',
         all(k in exp_text for k in ('def add(', 'def vote(', 'def edit('))
         and '"upvote"' in exp_text and '"downvote"' in exp_text),
        ('agentctl 暴露 experience add/ls/vote/edit',
         '"experience"' in ctl_text
         and all(('add_parser("%s"' % n) in ctl_text for n in ('add', 'ls', 'vote', 'edit'))),
        ('容量硬上限（Reflexion 3 条）',
         'MAX_ACTIVE = 3' in exp_text and 'while len(active) > max_active' in exp_text),
        ('淘汰理由进 ledger',
         '"evict"' in exp_text and '"add"' in exp_text
         and "LEDGER" in exp_text),
        ('没有验证回调就拒绝入库',
         'no verifier' in exp_text or 'verification_failed' in exp_text),
        ('写命令 proof 必须命中真实事件',
         'verify_against_events' in exp_text and 'no_matching_success_event' in exp_text),
        ('可疑内容隔离而非只记日志',
         'QUARANTINE' in exp_text and 'def suspicious_reasons' in exp_text
         and '_quarantine(data_dir' in exp_text),
        ('EDIT 重新验证，不能绕过入库门槛',
         "def edit(" in exp_text and 'no_verifier' in exp_text),
        ('B7 三项统计齐全（错误率 / 轮数 / 人工改写）',
         "perToolErrorRate" in exp_text and "turnsPerTask" in exp_text
         and "humanRewriteRate" in exp_text
         and "add_parser(\"stats\"" in ctl_text),
        ('统计有 audit 回退，不报假零',
         'event_rows' in exp_text and 'if not event_rows' in exp_text),
    ]:
        if not cond:
            exp_missing.append(label)
    if exp_missing:
        for label in exp_missing:
            fail('跨会话经验缺：' + label)
    else:
        ok('经验池先验证后入库 + 隔离/淘汰/ledger；统计覆盖错误率、轮数、人工改写')

    print()
    if g_fail:
        print(f'失败 {g_fail} 项')
        return 1
    print('全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())