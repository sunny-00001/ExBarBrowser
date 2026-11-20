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

不需要编译，纯文本对账。用法：python tools/check_ai_meta.py
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMMANDS = ROOT / 'src/commands.cpp'
HANDLERS_GLOB = 'app_commands*.cpp'

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
    'FsCopyTo': 'App::TransferTargets',
    'FsMoveTo': 'App::TransferTargets',
    # OpTopN/OpDiskUsage 的 regR 只是一行转发，参数在 RegisterFileCommands
    # 里的 scanImpl lambda 中读取（两条共用同一份实现）。
    'OpTopN': 'RegisterFileCommands::scanImpl',
    'OpDiskUsage': 'RegisterFileCommands::scanImpl',
}

KNOWN_NO_TARGET = {
    'CmdCut', 'CmdDelete', 'CopyToFolder', 'MoveToFolder', 'BatchRename',
    'PathCopyLink',
}


def fail(msg):
    global g_fail
    print(f'  ERROR  {msg}')
    g_fail += 1


def ok(msg):
    print(f'  ok     {msg}')


# 形如： {CmdId::X, L"X", L"Group", L"标签栏", L"Title", true,
#         Effects{0, 0, 0, 0, 0, Scope::App}, "{...}", true, false[, Requirements{...}]},
# 末尾的 Requirements 是可选的，所以不锚定 '}'，只锚定到 needsSelection 字段。
ENTRY = re.compile(
    r'\{CmdId::(\w+),\s*L"([^"]*)",\s*L"([^"]*)",\s*L"([^"]*)",\s*L"([^"]*)",\s*'
    r'(true|false),\s*Effects\{(\d), (\d), (\d), (\d), (\d), Scope::(\w+)\},\s*'
    r'"((?:[^"\\]|\\.)*)",\s*(true|false),\s*(true|false)'
)


def parse_commands():
    text = COMMANDS.read_text(encoding='utf-8')
    out = []
    for m in ENTRY.finditer(text):
        (cid, name, group, scopeui, title, impl,
         ro, de, idem, ow, undo, sc, schema, exposed, sel) = m.groups()
        out.append({
            'id': cid, 'name': name, 'group': group, 'title': title,
            'implemented': impl == 'true',
            'readOnly': ro == '1', 'destructive': de == '1',
            'idempotent': idem == '1', 'openWorld': ow == '1',
            'undoable': undo == '1', 'scope': sc,
            'schema': schema.replace('\\"', '"').replace('\\\\', '\\'),
            'exposed': exposed == 'true', 'needsSelection': sel == 'true',
            'requires': [],
        })
    # 第二遍：取 Requirements{...}（可选字段，可能不存在）
    for c in out:
        m = re.search(r'CmdId::' + c['id'] + r'\b[^\n]*?Requirements\{([^}]*)\}', text)
        if m:
            c['requires'] = [x.strip() for x in m.group(1).split(',') if x.strip()]
    return out


def collect_arg_key_usage():
    """扫所有 handler 分片，收集每个命令 handler 里实际取用了哪些参数键。

    handler 写作 reg(CmdId::X, [..](const Args& args) { ... args.Int(L"preset") ... })
    所以按 reg(CmdId::X 切段，在段内抓 args.Int/Str/Bool("<键>")。
    """
    usage = {}
    for path in sorted((ROOT / 'src').glob(HANDLERS_GLOB)):
        text = path.read_text(encoding='utf-8')
        # reg( 与 regR( 都要扫：后者是"带返回值"的注册（第 5 步引入）。
        parts = re.split(r'\breg(?:R)?\(CmdId::(\w+)', text)
        # parts[0] 是前置内容，之后是 (id, body, id, body, ...)
        for i in range(1, len(parts) - 1, 2):
            cid = parts[i]
            body = parts[i + 1]
            keys = set(re.findall(r'args\.(?:Int|Str|Bool|StrArray)\(L"([^"]+)"', body))
            usage.setdefault(cid, set()).update(keys)
    return usage


def main():
    cmds = parse_commands()
    if not cmds:
        fail('命令表解析为空 —— 文件格式变了？先修本脚本的正则')
        return 1
    print(f'命令表：{len(cmds)} 条')

    exposed = [c for c in cmds if c['exposed']]
    print(f'其中 aiExposed：{len(exposed)} 条\n')

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
    for c in exposed:
        try:
            v = json.loads(c['schema'])
            if not isinstance(v, dict):
                fail(f'{c["name"]} 的 argsSchema 不是 JSON 对象：{c["schema"][:60]}')
                bad += 1
            else:
                schemas[c['name']] = v
        except json.JSONDecodeError as e:
            fail(f'{c["name"]} 的 argsSchema 非法 JSON：{e}')
            bad += 1
    if not bad:
        ok(f'{len(exposed)} 条全部合法')

    print('\n3. schema 声明的参数必须被 handler 真正取用')
    usage = collect_arg_key_usage()
    by_name = {c['name']: c for c in exposed}
    # 公共实现里读的参数（INDIRECT_ARGS 登记的）
    # 公共实现可能在任一 app_commands*.cpp 分片里（RegisterFileCommands 在
    # app_commands_files.cpp，TransferTargets 在 app_commands.cpp）。
    shared_src = ''
    for path in sorted((ROOT / 'src').glob('app_commands*.cpp')):
        shared_src += path.read_text(encoding='utf-8')
    shared_keys = set(re.findall(r'args\.(?:Int|Str|Bool|StrArray|Has)\(L"([^"]+)"', shared_src))
    bad = 0
    for name, schema in schemas.items():
        declared = set((schema.get('properties') or {}).keys())
        if name in INDIRECT_ARGS:
            used = shared_keys
        else:
            used = usage.get(by_name[name]['id'], set())
        unused = declared - used
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
        ok('全部显式')

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
            declared = set((json.loads(c['schema']).get('properties') or {}).keys())
            if not declared:
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
    fs = [c for c in cmds if c['name'].startswith('Fs')]
    bad = 0
    for c in fs:
        props = set((json.loads(c['schema']).get('properties') or {}).keys())
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
        'CmdRename': 'FsRename',
        'CmdDelete': 'FsDelete',
        'CopyToFolder': 'FsCopyTo',
        'MoveToFolder': 'FsMoveTo',
        'CmdOpen': 'FsOpen',
        'CmdOpenInNewTab': 'FsOpenInNewTab',
        'CmdOpenInNewWindow': 'FsOpenInNewWindow',
        'CmdCut': 'FsCut',
        'CmdCopy': 'FsCopy',
        'CmdProps': 'FsProperties',
    }
    names = {c['name'] for c in cmds}
    impl = {c['name'] for c in cmds if c['implemented']}
    # 注意：这里遍历 COVERS 全体，不筛 KNOWN_NO_TARGET。
    # 早先多加了 `if old in KNOWN_NO_TARGET` 这一层，结果 CmdRename/CmdDelete
    # 明明在 COVERS 里却从不被打印 —— 覆盖情况看起来比实际更差。
    for old, new in COVERS.items():
        if new in names and new in impl:
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
    # 判定方式：每个条目在 commands.cpp 里固定一行，字符串字面量依次是
    #   name / group / scopeUi / title / argsSchema [/ outputSchema]
    # 所以"6 个字面量"等价于"声明了 outputSchema"。这比正则去数 schema 内容可靠。
    src_text = ''
    for path in sorted((ROOT / 'src').glob(HANDLERS_GLOB)):
        src_text += path.read_text(encoding='utf-8')
    result_regs = set(re.findall(r'(?:regR|RegisterWithResult)\(CmdId::(\w+)', src_text))

    cmd_text = COMMANDS.read_text(encoding='utf-8')
    bad = 0
    with_output = []
    for line in cmd_text.splitlines():
        m = re.match(r'\s*\{CmdId::(\w+)', line)
        if not m:
            continue
        cid = m.group(1)
        lits = re.findall(r'"(?:[^"\\]|\\.)*"', line)
        if len(lits) >= 6:
            with_output.append(cid)
            if cid not in result_regs:
                fail(f'{cid} 声明了 outputSchema 但没有用 RegisterWithResult 注册'
                     f'（结果永远是空的）')
                bad += 1
    if not bad and with_output:
        ok(f'{len(with_output)} 条带 outputSchema 的命令都已正确注册：{with_output}')
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
        # 命令名从"条目行前两个字段"取，不依赖尾部结构 ——
        # 否则尾部一坏，别名检查会连带误报"源命令不存在"，把真正的
        # 问题（字段漏写）埋在假错误里。
        legacy_names = set()
        for ln in COMMANDS.read_text(encoding='utf-8').splitlines():
            mm = re.match(r'\s*\{CmdId::\w+,\s*L"(\w+)"', ln)
            if mm:
                legacy_names.add(mm.group(1))
        dotted = {}
        bad = 0
        # 11a. 每个别名都要指向真实命令
        for legacy, dot in aliases:
            if legacy not in legacy_names:
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
        covered = {a[0] for a in aliases}
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
    missing = [f for f in required_fields if f'o.Set(L"{f}"' not in ai_cpp]
    if missing:
        fail(f'ToolsToJson 缺少字段：{missing}')
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
    # 判据：每个条目的尾部必须显式写出 <aiExposed>, <needsSelection>，
    # 不能靠默认值。这是纯文本检查，但对这类静默失效最有效。
    cmd_lines = COMMANDS.read_text(encoding='utf-8').splitlines()
    bad = []
    total = 0
    for line in cmd_lines:
        if not re.match(r'\s*\{CmdId::\w+,', line):
            continue
        total += 1
        # 尾部形状：<aiExposed>, <needsSelection>
        #            [, Requirements{...}]
        #            [, "outputSchema"]        <- 声明了返回值时
        # 三种尾部都合法；关键是**不能什么都不写**（靠默认值静默填）。
        if not re.search(r'(?:true|false),\s*(?:true|false)\s*'
                         r'(?:,\s*Requirements\{[^}]*\})?\s*'
                         r'(?:,\s*"(?:[^"\\]|\\.)*")?\s*\},?\s*$', line):
            bad.append(re.search(r'CmdId::(\w+)', line).group(1))
    if bad:
        fail(f'{len(bad)} 条缺少显式的 aiExposed/needsSelection：{bad}')
    else:
        ok(f'{total} 条全部显式声明 aiExposed/needsSelection')

    print('\n14. 暴露决策的合理性')
    # aiExposed=true 意味着机器能直接驱动这条命令。判据：
    #   a. 未实现的不能暴露（会在 tools/list 里出现但调不动）
    #   b. 依赖选中项的**不该**暴露 —— 机器无法指定"选哪个"，
    #      暴露出去就是"AI 以为删了 A，实际删了 B"
    #   c. 纯人机 UI（弹菜单/开关面板/退出程序）暴露没有意义
    #
    # 例外名单：确有理由的逐条登记，让"为什么它在这儿"可见。
    UI_ONLY = {
        'NavViewMenu',      # 弹视图菜单，机器调了得到的是一个没人看的菜单
        'QuitExplorer',     # 关程序不该由机器替人决定
        'FolderOptions',    # 弹系统对话框
        'MapDrive',         # 弹系统对话框
        'UnmapDrive',       # 弹系统对话框
        'CommandPalette',   # 调色板是给人键入命令用的
        'HotkeyManager',    # 快捷键配置面板
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
        exposed = [c for c in cmds if c['exposed']]
        ok(f'{len(exposed)} 条暴露的命令都合理（已实现、不依赖选中、非纯 UI）')

    unexp_impl = [c for c in cmds if c['implemented'] and not c['exposed']]
    if unexp_impl:
        print(f'  说明  {len(unexp_impl)} 条已实现但未暴露：')
        for c in unexp_impl:
            why = '依赖选中' if c['needsSelection'] else ('纯人机 UI' if c['name'] in UI_ONLY else '待决')
            print(f'        [{why}] {c["name"]}')

    print()
    if g_fail:
        print(f'失败 {g_fail} 项')
        return 1
    print('全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
