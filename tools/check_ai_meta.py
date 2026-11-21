# -*- coding: utf-8 -*-
"""AI 元数据静态对账（架构规则 R18 / R20）。

检查命令表与实现是否自洽 —— 这些错误在运行时只会表现为
"AI 说工具不存在"或"参数被静默丢弃"，很难查：

  1. 每个 aiExposed 的命令：implemented 必须为 true
  2. 每个 aiExposed 的命令：argsSchema 必须是合法 JSON 对象
  3. schema 声明的参数必须能在 handler 里找到取用（args.Int/Str/Bool("<键>")）
     —— 否则就是"声明了参数但没实现"，调用方传了等于白传
  4. needsSelection 的命令不应暴露（AI 无法用参数指定目标）
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
COMMANDS = ROOT / 'src/commands.cpp'
HANDLERS_GLOB = 'app_commands*.cpp'
KEYMAP_JSON = ROOT / 'data/keymap.json'
INPUT_ROUTER = ROOT / 'src/input_router.cpp'
KEYMAP_CPP = ROOT / 'src/keymap.cpp'

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
    # OpTopN/OpDiskUsage 的 regR 只是一行转发，参数在 RegisterFileCommands
    # 里的 scanImpl lambda 中读取（两条共用同一份实现）。
    'tool.topn': 'RegisterFileCommands::scanImpl',
    'tool.diskusage': 'RegisterFileCommands::scanImpl',
}

KNOWN_NO_TARGET = {
    'file.cut', 'file.delete', 'file.copy.to', 'file.move.to', 'file.rename.batch',
    'path.copy.link',
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
    'tab.color': '标签右键菜单「设置标签页颜色」→ 二级色板（参数 color 来自选中的那一行）'
                 '→ WM_APP_TAB_COLOR → App::ShowTabColor → App::SetTabColor',
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
    'tool.stat': '状态栏「选中容量」段 + 详细信息视图列展示同一组字段'
                 '（名称/大小/类型/隐藏/只读）',

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

    # 设置面板（视图菜单「设置…」）：列出键表里每一项与当前值。
    # 值在渲染时直读键表（一屏几十行，不能每行发一次命令），但**选中某项后**
    # 走 config.get 读最新值、config.set 写回 —— 与 AI 完全同一条路径、同一份校验。
    # 参数 key 就是被点中的那一行的键名（settings.json 里的名字）。
    'config.get': '视图菜单「设置…」→ 设置面板（settings_panel.cpp）：'
                  '点某一项时先调 config.get 读最新值（参数 key 即该行的键名）',
    'config.set': '视图菜单「设置…」→ 设置面板：布尔项点一下翻转、数值/字符串弹输入框、'
                  '数组填逗号分隔，一律调 config.set（参数 key/value 来自该行与输入框）',
}

# 机器专用查询：人能通过命令面板触发，但拿到的是 JSON（写进审计日志），
# 界面上没有可见结果。它们**不是**违规 —— 这类命令存在的意义就是把结构化
# 数据交给 agent（第 5 步的 OpListDir/OpStat 就是为此而加）。登记在这里
# 是为了让"人点了看不到东西"这件事是已知的，而不是被当成 bug 反复排查。
AI_ONLY_QUERIES = {
    'tool.list': '返回当前目录的条目数组；人的等价物是直接看文件列表',
    'tool.topn': '返回最大的 N 项；界面上没有"按体积排序"的视图',
    'tool.diskusage': '返回目录总占用；界面上没有这个读数',
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
    '标签·向左移动': 'tab.move.left',
    '标签·向右移动': 'tab.move.right',
    '标签·移动到开头': 'tab.move.first',
    '标签·移动到末尾': 'tab.move.last',
    '标签·重新打开关闭的标签': 'tab.reopen',
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
        name, raw, lit = m.group(1), m.group(2), m.group(3)
        if raw is not None:
            consts[name] = m.group(3)
        else:
            consts[name] = lit.replace('\\"', '"').replace('\\\\', '\\')
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


def parse_commands():
    raw = COMMANDS.read_text(encoding='utf-8')
    text = _strip_comments(raw)
    consts = _string_constants(text)

    m = re.search(r'std::vector<CommandDef>\s+kTable\s*=\s*\{', text)
    if not m:
        return [], text, consts
    start = m.end() - 1
    inner, _ = _html_block(text, start)
    if inner is None:
        return [], text, consts

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
        schema = _string_literal(fields[7], consts) if len(fields) > 7 else None
        exposed = fields[8].strip() == 'true' if len(fields) > 8 else False
        sel = fields[9].strip() == 'true' if len(fields) > 9 else False

        # 尾部可选字段按**位置**解析，不按"最后一个字符串"猜。
        #
        # struct 声明顺序固定为：requires_ / outputSchema / assertion。
        # 关键：`nullptr` 是 outputSchema 的显式占位（"这条不产出结果"），
        # 它必须占掉一个位置 —— 否则后面的 assertion 会顶上来被当成结果 schema，
        # 报出"outputSchema 非法 JSON"这种指向错误的假错误。
        #
        # 例：
        #   ..., false, false, Requirements{}, "outputSchema", "assert"    → 12 字段
        #   ..., false, false, Requirements{}, nullptr,       "assert"     → 13 字段
        reqs, outschema, assertion = [], None, None
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
        if pos < len(tail):
            v = _string_literal(tail[pos], consts)
            if v is not None:
                assertion = v

        out.append({
            'id': cid, 'name': name, 'group': group, 'title': title,
            'scopeUi': scopeui, 'implemented': impl,
            'readOnly': ro == '1', 'destructive': de == '1',
            'idempotent': idem == '1', 'openWorld': ow == '1',
            'undoable': undo == '1', 'scope': scope,
            'schema': schema, 'exposed': exposed, 'needsSelection': sel,
            'requires': reqs, 'outputSchema': outschema, 'assertion': assertion,
            'nfields': len(fields),
        })
    return out, text, consts


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

    print('\n4. needsSelection 的命令不应暴露（AI 无法指定目标）')
    bad = 0
    for c in cmds:
        if c['needsSelection'] and c['exposed']:
            fail(f'{c["name"]} 依赖选中项却又暴露给 AI，目标不可表达')
            bad += 1
    if not bad:
        ok('无冲突')

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
        if c['needsSelection'] and 'HasSelection' not in c['requires']:
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
    COVERS = {
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
    # 判据有两条，缺一不可：
    #   a. 有 outputSchema 的必须用 regR/RegisterWithResult 注册；
    #   b. outputSchema 本身必须是合法 JSON 对象（否则客户端校验必失败）。
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
    ai_cpp = (ROOT / 'src/ai_api.cpp').read_text(encoding='utf-8')
    required_fields = ['name', 'legacyName', 'title', 'description',
                       'annotations', 'inputSchema']
    missing_fields = [f for f in required_fields if f'o.Set(L"{f}"' not in ai_cpp]
    if missing_fields:
        fail(f'ToolsToJson 缺少字段：{missing_fields}')
    else:
        ok(f'ToolsToJson 输出 {len(required_fields)} 个必需字段')

    # inputSchema / outputSchema 必须是解析后的对象，不能塞字符串
    if 'json::Parse(t.argsSchema, schema' in ai_cpp and 'o.Set(L"inputSchema", schema)' in ai_cpp:
        ok('inputSchema 解析成对象（不是字符串）')
    else:
        fail('inputSchema 没有解析成对象 —— MCP 要求它是 JSON 对象')

    # annotations 四要素
    ann_fields = ['readOnlyHint', 'destructiveHint', 'idempotentHint', 'openWorldHint']
    miss_ann = [f for f in ann_fields if f'ann.Set(L"{f}"' not in ai_cpp]
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
        if c['needsSelection']:
            fail(f'{c["name"]} 暴露给 AI 但依赖当前选中（机器无法指定目标）')
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

    # 命令面板覆盖哪些命令：读 ShowCommandPalette 的实现。
    #
    # 判据变过一次，原因值得记下来：
    #   旧判据 = "筛掉未实现与有必填参数的命令，其余空参调用"。
    #   它防的是真危险（tab.rename/tab.color 空参调用会静默清别名/颜色），
    #   但代价是把 config.get / config.set / file.rename 这类带参命令
    #   在人类侧整条抹掉 —— 只有 AI 能触发，正好违反本项要守护的 R20。
    #   而且"面板跳过"根本不是防静默清空的唯一办法：按 schema 追问参数再调用，
    #   同样不会空参调用，还多了人类入口。
    #
    # 所以现在的判据是两条，任一不成立都说明"带参命令又被人类侧漏掉了"：
    #   ① 面板遍历整张命令表（只筛 !implemented），并且**两条路都在**：
    #      无必填参数走 Args::Empty()，有必填参数走 Args::FromJson(收集结果)。
    #   ② 参数收集器从 argsSchema 里读 required/properties —— 不允许硬编码
    #      某几条命令的参数名，否则新增命令又得改面板（回到打补丁）。
    app_commands = (ROOT / 'src/app_commands.cpp').read_text(encoding='utf-8')
    palette_body = _body_of(app_commands, 'void App::ShowCommandPalette()')
    palette_all = ('CommandTable()' in palette_body
                   and '!d.implemented' in palette_body
                   and 'Args::Empty()' in palette_body
                   and 'Args::FromJson(' in palette_body)
    if not palette_all:
        fail('命令面板必须"列出所有已实现命令"，且无必填参数走空参、'
             '有必填参数走按 schema 收集后的参数 —— 否则带参命令在人类侧不可达（R20）')
        return 1
    collect_body = _body_of(app_commands, 'CollectParamsInteractive')
    if not ('"required"' in collect_body and '"properties"' in collect_body):
        fail('命令面板的参数收集器必须从 argsSchema 读 required/properties，'
             '不能硬编码某几条命令的参数名')
        return 1

    def required_of(c):
        return sorted((schemas[c['name']].get('required') or []))

    def param_source(c):
        """B 路：返回一条能把必填参数递进去的人类路径说明，找不到返回 None。"""
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

    # 16b. 覆盖率：扫菜单源码里**实际存在**的文案，凡是没登记的都必须报出来。
    #
    # 只靠上面那张手工表时，新增一个菜单项却忘了登记，检查会静默通过 ——
    # 那正是「菜单里写了就完事」再次长出来的方式。这里反查源码：
    # 菜单文案是字符串字面量，抽得出来；抽得到就必须在表里有交代。
    scan_sources = [
        ('标签', 'src/tab_strip_actions.cpp'),
        ('书签', 'src/bookmark_bar.cpp'),
    ]
    registered = set(MENU_ACTIONS.keys())
    unregistered = []
    for area, rel in scan_sources:
        src = (ROOT / rel).read_text(encoding='utf-8', errors='replace')
        # 只认「.Add(L"文案", kXxxMenuYyy)」这种「菜单项 + action」的写法。
        for m in re.finditer(r'\.Add\(\s*L"([^"]+)"\s*,\s*(k\w+Menu\w+)', src):
            label = m.group(1).replace('...', '').strip()
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
    ai_cpp = (ROOT / 'src' / 'ai_api.cpp').read_text(encoding='utf-8', errors='replace')
    readback_missing = []
    for field, cmds_writing in WRITABLE_STATE:
        in_struct = re.search(rf'\b{field}\b', ai_h) is not None
        in_json = re.search(rf'L"{field}"', ai_cpp) is not None
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
    for m2 in re.finditer(r'v\.Set\(L"(\w+)"', ai_cpp):
        state_fields.add(m2.group(1))
    bad_assert = []
    checked_assert = 0
    for c in cmds:
        a = c.get('assertion')
        if not a:
            continue
        checked_assert += 1
        if '=' not in a:
            bad_assert.append((c['name'], a, '缺少 ='))
            continue
        lhs, rhs = a.split('=', 1)
        lhs, rhs = lhs.strip(), rhs.strip()
        if lhs not in state_fields:
            bad_assert.append((c['name'], a, '左值 %s 不在 exbar/state 里' % lhs))
        if rhs.startswith('$arg:'):
            key = rhs[5:]
            if key.endswith('.length'):
                key = key[:-len('.length')]
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
    srv = (ROOT / 'src/ai_server.cpp').read_text(encoding='utf-8', errors='replace')
    api = (ROOT / 'src/ai_api.cpp').read_text(encoding='utf-8', errors='replace')
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
        ('目录切换早于单实例互斥', main_cpp.find('SetProfileDir(') < main_cpp.find('CreateMutexW(')),
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

    print()
    if g_fail:
        print(f'失败 {g_fail} 项')
        return 1
    print('全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())