#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令目录静态审计（CI / 提交前跑一次，无窗口依赖）。

检查四件事，任何一条不满足都说明"键位/面板点得到但没实现"：
  1. data/keymap.json 里出现的每条命令（hotkeys + sequences）都在命令表里
     （src/commands_table.cpp 用户操作域 + src/commands_table_tools.cpp 工具域）；
  2. 表里 implemented=true 的命令，在 src/app_commands.cpp 里都有 reg(CmdId::X) 处理器；
  3. 表里 implemented=false 的命令不能有处理器（否则是"实现了却标成未实现"）；
  4. keymap 里的键位不能绑到 implemented=false 的命令上（否则按了没反应、也没有提示）。
     确属"先占位、功能后补"的要写进下面的 PENDING，并写明原因。

用法：python tools/check_commands.py     # 退出码 0 = 通过
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# 命令表本体按作用域分在两个文件里（用户操作域 / 工具·Agent 域），
# 由 commands_table.h 提供共用常量。读表要读**全部**两段 —— 少读一段
# 会让那一段的命令在校验器眼里"不存在"（键位里绑了它们却报未注册）。
TABLE_SRCS = [
    ROOT / "src" / "commands_table.cpp",
    ROOT / "src" / "commands_table_tools.cpp",
]
HANDLERS_GLOB = "app_commands*.cpp"   # 处理器按域分片后不再只有一个文件
KEYMAP = ROOT / "data" / "keymap.json"

# 键位先占位、功能后补：这些 setting 绑的命令尚未实现，是有意为之。
# 新增项必须写明原因，否则应视为违规（键位按了没反应，用户不会知道为什么）。
#
# 登记不是免死金牌：每次改动都要回头看这条到底还该不该留着。命令一旦实现，
# 把对应项从这里删掉 —— 否则"实现了却仍被当成占位"会让这条检查失去意义。
PENDING = {
    # 选择族与撤销已于 2026-11-21 实现：
    #   · 选择：shellops::SelectWhere / SelectByPattern（Ctrl+I / Ctrl+Shift+F /
    #     Ctrl+Alt+D / Ctrl+Shift+K 现在真的有效）；
    #   · 撤销：shellops::Undo 把 Ctrl+Z 转发给 shell 视图自己的撤销栈
    #     （所有落盘操作都带 FOF_ALLOWUNDO，已在系统栈里）。
    # 批量重命名已于 2026-11-21 实现（Ctrl+Shift+R）：不接 PowerRename，
    # 只做"模式串 + 预览 + 全部先校验再动手"的最小集合（filetools::PlanBatchRename），
    # 因为外部依赖不能保证存在，而"按了没反应"是当时要解决的问题本身。
    # 搜索框与 Ctrl+F 已于 2026-11-21 落地（NavBar::FocusSearch + search-ms）。
    # 工具集三条已于 2026-11-21 实现：DupFinder / FileHash / FolderCompare
    # （file_hash.cpp + file_tools.cpp，预算内同步扫描 + 只读报告窗口）。
    # 这里原先的理由写着"需 helper 进程 + IPC"，实现时发现不必要：
    # 预算是显式的（timedOut/complete 如实上报），单文件哈希本来就必须读完整文件，
    # 用 helper 换来的只是"扫描时界面不卡"，代价是一整套 IPC 与生命周期管理。
    # 代价是预算耗尽会少扫一部分 —— 报告里写清楚了，比假死或假数字好。
    # AI 面板（Ctrl+Alt+A）的键位已删（2026-11-21）。原因不是"还没做"，而是
    # **形态就不对**：本工程的 AI 是外部 agent 通过命名管道驱动窗口（MCP 客户端），
    # 窗口内没有也不需要"聊天"；真要给用户看的是"管道名/令牌/最近调用"，
    # 那是另一条命令，不该占用 AiTogglePanel 这个名字。
    # 留着一个永远不会响应的 Ctrl+Alt+A，等于对用户撒谎。
    # 已删键位（2026-11-21）：hotkeyMergeAllWindows（Ctrl+Alt+W）与
    # hotkeyToggleCheckboxes（Ctrl+Alt+M）。这两条不是"待补"而是"本工程做不到"：
    #   · 合并窗口 —— 单窗口模型，没有 explorer 标签可合并（PARITY §3.1）；
    #   · 复选框   —— shell 视图无公开接口（DECISIONS D2）。
    # 留着键位等于对用户撒谎（按了没反应且永远不会有），所以连键位一起删掉。
}


def strip_comments(text):
    """去掉整行/行尾 // 注释，避免文档里的示例被当成真实注册。"""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def EnumOrder():
    """按声明顺序读出 commands_ids.h 里 `enum class CmdId` 的全部标识符。

    只认 word 形态的 token，跳过显式赋值（`None = 0`）与注释。
    这是 CmdId 数值的真实来源 —— 与命令表行的顺序无关。
    （枚举本体在 src/commands_ids.h，与命令契约 commands.h 分开。）
    """
    src = (ROOT / "src" / "commands_ids.h").read_text(encoding="utf-8")
    body = re.search(r"enum class CmdId\s*:\s*int\s*\{(.*?)\n\};", src, re.S)
    if not body:
        return []
    body = re.sub(r"//.*", "", body.group(1))
    out = []
    for tok in body.split(","):
        tok = tok.strip()
        if not tok:
            continue
        m = re.fullmatch(r"(\w+)\s*(?:=\s*\d+)?", tok)
        if not m:
            continue
        name = m.group(1)
        if name == "None":      # 显式 = 0 的哨兵，不算命令
            continue
        out.append(name)
    return out


# ---- 快捷键 chord 的 Python 复刻（与 src/chord.cpp 同口径）----
#
# 【为什么要复刻而不是抄一份名字表】运行时判重用的是 chord 对象
# （mods 位 + 虚拟键码），不是字符串。于是 "Ctrl+A" 与 "ctrl+a"、
# "Ctrl+Alt+Delete" 与 "Ctrl+Alt+Del" 在运行时是**同一个 chord**，
# 但在字符串上看着不同 —— 只比字符串的检查会漏掉这两类真冲突。
#
# 下面这张表与 src/chord.cpp 的 kKeys 一一对应（虚拟键码取 Win32 常量值）。
VK_KEYS = {
    "backspace": 0x08, "tab": 0x09, "enter": 0x0D, "return": 0x0D,
    "esc": 0x1B, "escape": 0x1B, "space": 0x20,
    "insert": 0x2D, "ins": 0x2D, "delete": 0x2E, "del": 0x2E,
    "home": 0x24, "end": 0x23,
    "pgup": 0x21, "pageup": 0x21, "pgdn": 0x22, "pagedown": 0x22,
    "left": 0x25, "←": 0x25, "right": 0x27, "→": 0x27,
    "up": 0x26, "↑": 0x26, "down": 0x28, "↓": 0x28,
    "capslock": 0x14, "numlock": 0x90, "scrolllock": 0x91,
    "pause": 0x13, "printscreen": 0x2C, "prtsc": 0x2C,
    "apps": 0x5D,
    "num0": 0x60, "num1": 0x61, "num2": 0x62, "num3": 0x63, "num4": 0x64,
    "num5": 0x65, "num6": 0x66, "num7": 0x67, "num8": 0x68, "num9": 0x69,
    "num*": 0x6A, "num+": 0x6B, "num-": 0x6D, "num.": 0x6E, "num/": 0x6F,
}

# 单字符按键（src/chord.cpp 的 VkFromName 第一段）。
VK_SYMBOLS = {
    "+": 0xBB, "-": 0xBD, ",": 0xBC, ".": 0xBE, "/": 0xBF, ";": 0xBA,
    "'": 0xDE, "[": 0xDB, "]": 0xDD, "\\": 0xDC, "`": 0xC0,
}

MOD_CTRL, MOD_SHIFT, MOD_ALT, MOD_WIN = 1, 2, 4, 8

# 修饰键别名归一（与 chord::Parse 的 if/else 链同序）。
# 注意 "menu" 在 chord::Parse 里**先**当修饰键判（→ Alt），所以永远不是 VK_APPS。
MOD_ALIASES = {
    "ctrl": MOD_CTRL, "control": MOD_CTRL,
    "shift": MOD_SHIFT,
    "alt": MOD_ALT, "menu": MOD_ALT,
    "win": MOD_WIN, "windows": MOD_WIN, "super": MOD_WIN,
}

# 系统保留组合：InputRouter::BuildBindings 的 add() 直接跳过它们
# （chord::IsReserved），所以撞上它们**不算冲突**（谁也抢不到）。
RESERVED = {
    (MOD_ALT, 0x73),    # Alt+F4
    (MOD_ALT, 0x09),    # Alt+Tab
    (MOD_CTRL, 0x1B),   # Ctrl+Esc
    (MOD_CTRL | MOD_ALT, 0x2E),  # Ctrl+Alt+Del
}


def _vk_of(name):
    """按键名 → 虚拟键码；认不出返回 0（与 VkFromName 的 0 语义一致）。

    【单字符要"落穿"到别名表】src/chord.cpp 的 VkFromName 里，
    `if (name.size() == 1)` 那段对没命中的字符是 `default: break` —— **不返回**，
    会继续往下走 kKeys 表。而 kKeys 表里有 "←" "→" "↑" "↓" 这四个单字符。
    所以 "Alt+←" 在运行时能解析成 VK_LEFT。
    如果这里在单字符分支直接 return 0，就会漏掉全部方向键组合
    （kSysKeys 里就有 "Alt+←" / "Alt+→" / "Alt+↑"）。
    """
    if len(name) == 1:
        c = name[0]
        if "a" <= c <= "z":
            return ord(c.upper())
        if "0" <= c <= "9":
            return ord(c)
        if c in VK_SYMBOLS:
            return VK_SYMBOLS[c]
        # 不 return —— 落到下面的别名表（与 C++ 的 default: break 同义）
    if name in VK_KEYS:
        return VK_KEYS[name]
    m = re.fullmatch(r"f(\d+)", name)
    if m and 1 <= int(m.group(1)) <= 24:
        return 0x70 + int(m.group(1)) - 1
    return 0


def parse_chord(text):
    """"Ctrl+Shift+A" → (mods, vk) 的规范形式；解析不出来返回 None。

    与 src/chord.cpp 的 chord::Parse 同口径：trim → 按 '+' 切 → 逐段小写 →
    修饰键走别名表、其余走按键表；最后一个非修饰键即 vk。
    """
    s = (text or "").strip()
    if not s:
        return None
    parts, cur = [], ""
    i = 0
    while i < len(s):
        c = s[i]
        if c == "+":
            parts.append(cur)
            cur = ""
            if i + 1 == len(s):
                cur = "+"          # "Ctrl++"
        else:
            cur += c
        i += 1
    if cur:
        parts.append(cur)

    mods, vk = 0, 0
    for part in parts:
        p = part.strip().lower()
        if not p:
            continue
        if p in MOD_ALIASES:
            mods |= MOD_ALIASES[p]
        else:
            v = _vk_of(p)
            if not v:
                return None
            vk = v
    if not vk:
        return None
    return (mods, vk)


def _read_sys_keys():
    """src/keymap.cpp 的 kSysKeys 表：setting → 系统原生键。

    这张表是 keymap.json 里"key 为空"的兜底 —— json 没写键的条目最终落到这里，
    所以它也是冲突源之一（查 json 单独一份是查不出来的）。
    """
    path = ROOT / "src" / "keymap.cpp"
    if not path.exists():
        return {}
    src = strip_comments(path.read_text(encoding="utf-8"))
    body = re.search(r"kSysKeys\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not body:
        return {}
    return dict(re.findall(r'\{L"([^"]*)",\s*L"([^"]*)"\}', body.group(1)))


def _read_extra_keys():
    """src/input_router.cpp 的 kExtra 表：应用内补充分配。

    在运行时它是**最后**才 add 的，而 add 是"先到先得" —— 所以它撞上
    前面任何一条（json / kSysKeys）时，是**它自己**被丢弃。
    """
    path = ROOT / "src" / "input_router.cpp"
    if not path.exists():
        return []
    src = strip_comments(path.read_text(encoding="utf-8"))
    body = re.search(r"kExtra\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not body:
        return []
    return re.findall(r'\{L"([^"]*)",\s*L"([^"]*)"\}', body.group(1))


def find_chord_conflicts(km):
    """返回冲突描述列表；空列表表示没有冲突。

    合成的顺序与运行时一致：json 的 hotkeys（空键落到 kSysKeys）→ kExtra。
    同一 chord 第二次出现即为冲突（先到先得，后一条被静默丢弃）。
    """
    sys_keys = _read_sys_keys()
    seen = {}                       # (mods, vk) → 来源描述

    def claim(chord, where):
        if chord is None:
            return
        mods, vk = chord
        if mods & MOD_WIN:
            return                 # Win 组合一律交系统，应用内抢不到
        if (mods, vk) in RESERVED:
            return                 # 系统保留键，谁也抢不到
        prev = seen.get((mods, vk))
        if prev:
            seen[(mods, vk)] = prev + " 与 " + where
        else:
            seen[(mods, vk)] = where

    for h in km.get("hotkeys", []):
        setting = h.get("setting", "")
        key = (h.get("key", "") or "").strip()
        if key == "-":
            continue               # 哨兵 = 用户显式解绑（src/config.h kHotkeyDisabled）
        if not key:
            key = sys_keys.get(setting, "")
        chord = parse_chord(key)
        claim(chord, f"{setting or '?'}（{h.get('command', '')}）← {key or '无键'}")

    for key, cmd in _read_extra_keys():
        chord = parse_chord(key)
        claim(chord, f"kExtra {cmd} ← {key}")

    return [f"{key_label(c)}：{desc}"
            for c, desc in sorted(seen.items()) if " 与 " in desc]


def key_label(chord):
    """把 (mods, vk) 还原成可读的组合串（仅用于报错信息）。"""
    mods, vk = chord
    parts = []
    if mods & MOD_CTRL:
        parts.append("Ctrl")
    if mods & MOD_SHIFT:
        parts.append("Shift")
    if mods & MOD_ALT:
        parts.append("Alt")
    if mods & MOD_WIN:
        parts.append("Win")
    rev = {v: k for k, v in VK_KEYS.items()}
    if vk in rev:
        parts.append(rev[vk])
    elif 0x70 <= vk <= 0x87:
        parts.append("F%d" % (vk - 0x70 + 1))
    elif 0x60 <= vk <= 0x69:
        parts.append("Num%d" % (vk - 0x60))
    elif 0x41 <= vk <= 0x5A:
        parts.append(chr(vk))
    elif 0x30 <= vk <= 0x39:
        parts.append(chr(vk))
    else:
        parts.append("VK_%02X" % vk)
    return "+".join(parts)


def main():
    table_src = "\n".join(
        strip_comments(p.read_text(encoding="utf-8")) for p in TABLE_SRCS
    )
    # 命令表条目现在带结构化元数据（Effects{...}/argsSchema/aiExposed/needsSelection/
    # Requirements{...}），argsSchema 是含大量 '}' 的 JSON 字符串、Effects 本身也是
    # 花括号 —— 跨行/跨括号的整条匹配极易失配。
    #
    # 改为**按行**匹配：每个条目在命令表里固定占一行，只取前 6 个字段
    # （id/name/group/scopeUi/title/implemented）。本脚本只关心这 6 个，
    # 尾部结构一概不管 —— 这样以后再加字段也不会把它弄坏。
    rows = []
    for line in table_src.splitlines():
        m = re.match(
            # 主名可能是点分名（win.quit / tab.new），所以第二组不能是 \w+
            r'\s*\{CmdId::(\w+),\s*L"([^"]+)",\s*L"([^"]*)",\s*L"([^"]*)",\s*L"([^"]*)",\s*(true|false),',
            line,
        )
        if m:
            rows.append(m.groups())
    if not rows:
        print("FAIL 没能从命令表（src/commands_table*.cpp）解析出条目")
        return 1

    ids = {r[0] for r in rows}
    names = {r[1] for r in rows}

    # 规则 C2/C3（docs/NAMING.md §4.3）：旧名永不删，且所有消费方都要认旧名。
    # 用户已有的 keymap.json 里写的是旧名（QuitExplorer），不能因为改名就失效。
    # 这里从 cmd_naming.cpp 读出 {旧名 → 点分名} 表，把旧名也算作合法名字 ——
    # 与运行时 FindCommand 的解析口径保持一致。
    alias_src = (ROOT / "src" / "cmd_naming.cpp").read_text(encoding="utf-8")
    aliases = re.findall(r'\{L"(\w+)",\s*L"([\w.]+)"\}', alias_src)
    for legacy, _dotted in aliases:
        names.add(legacy)
    # 处理器是按 CmdId 枚举名注册的（reg(CmdId::QuitExplorer, ...)），
    # 而表里的 name 可能是点分名（win.quit）—— 两边要先归一到 id 才能比对。
    name_to_id = {r[1]: r[0] for r in rows}
    implemented = {r[0] for r in rows if r[5] == "true"}
    table_ids = {r[0] for r in rows}

    handlers = set()
    # 处理器按域分片（app_commands.cpp / _tabs / _files / _misc），审计要看全部片。
    for path in sorted((ROOT / "src").glob(HANDLERS_GLOB)):
        # reg( 是普通注册；regR( 是"带结构化返回值"的注册（第 5 步引入）。
        # 两者都算"有处理器"，否则会误报"已实现但没处理器"。
        handlers |= set(re.findall(r"reg(?:R)?\(CmdId::(\w+)",
                                   strip_comments(path.read_text(encoding="utf-8"))))

    km = json.loads(KEYMAP.read_text(encoding="utf-8"))
    specs = [h.get("command", "") for h in km.get("hotkeys", [])]
    specs += [s.get("command", "") for s in km.get("sequences", [])]

    errors = []

    missing_in_table = []
    for spec in specs:
        name = spec.split(":", 1)[0]
        if name and name not in names:
            missing_in_table.append(spec)
    if missing_in_table:
        errors.append("keymap 引用了目录里不存在的命令（键位会静默消失）：" +
                      ", ".join(sorted(set(missing_in_table))))

    # ---- keymap 只绑已实现的命令 ----
    #
    # 上面那条只保证"名字在表里"，表里却有 40 多条 implemented=false。
    # name 只是"认得这个命令"，不等于"按下去有反应"：InputRouter 建表时对未实现的
    # 命令直接 continue（src/input_router.cpp 的 `!def->implemented` 分支），
    # 于是键位既不触发也不报错 —— 用户只看到"按了没反应"。
    #
    # 归一化要注意口径：keymap 里写的是旧名（SelectInvert），表里 name 是点分名
    # （file.select.invert），而 implemented 收集的是 CmdId 枚举名。所以
    # 旧名/点分名 → 都要先经别名表 + name_to_id 落到 CmdId，再问 implemented。
    legacy_to_dotted = dict(aliases)

    def to_cmd_id(name):
        """命令名（旧名 / 点分名）→ CmdId 枚举名；认不出返回 None。"""
        if name in name_to_id:                 # 点分名（表里的主名）
            return name_to_id[name]
        dotted = legacy_to_dotted.get(name)    # 旧名 → 点分名
        return name_to_id.get(dotted) if dotted else None

    # (setting 或 sequence:<seq>, 命令名)
    bindings = [(h.get("setting", ""), h.get("command", "")) for h in km.get("hotkeys", [])]
    bindings += [("sequence:" + s.get("seq", ""), s.get("command", ""))
                 for s in km.get("sequences", [])]
    unimpl, pending_hit = [], []
    for setting, spec in bindings:
        name = spec.split(":", 1)[0]           # "ViewMode:3" 这类内联参数先剥掉
        if not name:
            continue
        cid = to_cmd_id(name)
        # 认出了且已实现 → 正常；其余（认不出 / 标着未实现）都是"按了没反应"。
        # 认不出的情况上面 missing_in_table 已经报过，这里不重复计一遍。
        if cid is None or cid in implemented:
            continue
        if setting in PENDING:
            pending_hit.append((setting, name))
        else:
            unimpl.append(f"{setting}→{name}")
    if unimpl:
        errors.append("keymap 绑了未实现的命令（按了没反应）：" + ", ".join(sorted(unimpl)))

    # PENDING 里过期/写错的项也要报出来，否则这张白名单只会越来越长：
    #   - setting 在 keymap 里已不存在（改名或删键位了，登记项失去对象）；
    #   - 命令已经实现了却还挂在 PENDING（占位理由不成立了）。
    stale = []
    bound = {s for s, _ in bindings}
    for setting in sorted(PENDING):
        if setting not in bound:
            stale.append(f"{setting}（keymap 里没有这个 setting）")
            continue
        spec = next(c for s, c in bindings if s == setting)
        cid = to_cmd_id(spec.split(":", 1)[0])
        if cid is not None and cid in implemented:
            stale.append(f"{setting}（{cid} 已实现，可以摘掉了）")
    if stale:
        errors.append("PENDING 有失效登记（应删掉对应项）：" + ", ".join(stale))

    no_handler = sorted(n for n in implemented if n not in handlers)
    if no_handler:
        errors.append("标为已实现但没有处理器（点/按都没反应）：" + ", ".join(no_handler))

    dead_handler = sorted(handlers - implemented)
    if dead_handler:
        errors.append("有处理器但表里标为未实现（管理器会显示未实现）：" + ", ".join(dead_handler))

    dup_names = sorted({r[1] for r in rows if [x[1] for x in rows].count(r[1]) > 1})
    if dup_names:
        errors.append("命令名重复：" + ", ".join(dup_names))

    # ---- 规则 C4：序列键不得绑"有必填参数又没带内联参数"的命令 ----
    #
    # 【为什么这条必须机械检查】序列的派发是
    #   InputRouter::FeedSequence → InvokeByNameLegacy(cmd, L"") → ArgsFromLegacyArg(def, "")
    # 而 ArgsFromLegacyArg 收到空串直接返回 Args::Empty() —— 必填参数**永远是空的**。
    # 于是带必填参数的命令绑到序列上，每按一次都注定失败；更糟的是有三条
    # 在空参下会**静默改状态**而不是如实报错：
    #   · tab.color   空 color=0        → 把标签改成"无颜色"（用户看不出发生了什么）
    #   · win.move    空 x,y=0          → 把窗口挪到屏幕左上角
    #   · view.pane.split 空 percent=50 → 分栏比例被重置成一半
    # 这三条不是"按了没反应"，是"按了做了别的事" —— 比失败更坏。
    #
    # 合法形态只有两种：
    #   · 命令没有必填参数；
    #   · 命令规格带内联参数（"view.mode:3" / "ViewZoom:-1"）。
    # 有必填参数又要留在序列里的，唯一正解是补内联参数，不是往白名单里塞。
    #
    # 判据从数据里取（argsSchema 的 required），不看注释、不看命令名 ——
    # 新增命令时它自动生效，不需要有人记得来登记。
    #
    # 这里单独抽一份"CmdId → argsSchema 字符串"：上面 rows 只取了前 6 个字段
    # （够用且稳定），但"有没有必填参数"必须读 schema 本体。
    # 取法同样是按行 —— schema 是同一行里的第 7 个字段（kNoArgs 常量名也算），
    # 所以从行里切出 `, Effects{...}, "..."` 之后的那一段即可。
    schema_of = {}
    const_schema = {}
    # 表元数据常量（kNoArgs 等）定义在 commands_table.h：`inline constexpr const char* kNoArgs = R"({...})";`
    consts_src = strip_comments((ROOT / "src" / "commands_table.h").read_text(encoding="utf-8"))
    for m in re.finditer(r'constexpr\s+const\s+char\*\s+(\w+)\s*=\s*R"\((.*?)\)"', consts_src, re.S):
        const_schema[m.group(1)] = m.group(2)

    def unescape_c(text):
        """把表里的 C 字符串字面量转成原始 JSON 文本。

        【为什么必须做】命令表的 argsSchema 是 `"...\"required\":[...]"` 这种
        C 转义写法（JSON 里的引号在 C 源码里必须转义）。不去转义直接查
        `'"required"'` 永远不命中 —— 判据会**恒绿**，比不写这条更坏：
        它给出一个"已经查过了"的承诺，而实际什么都没查。
        """
        out = []
        i, n = 0, len(text)
        while i < n:
            c = text[i]
            if c == '\\' and i + 1 < n:
                nxt = text[i + 1]
                out.append({'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}.get(nxt, nxt))
                i += 2
                continue
            out.append(c)
            i += 1
        return ''.join(out)

    for line in table_src.splitlines():
        m_id = re.match(r'\s*\{CmdId::(\w+),', line)
        if not m_id:
            continue
        # Effects{...} 之后、下一个逗号分隔的字段就是 argsSchema。
        m_eff = re.search(r'Effects\{[^}]*\}\s*,\s*', line)
        if not m_eff:
            continue
        rest = line[m_eff.end():]
        m_schema = re.match(r'(?:R"\((.*?)\)"|"((?:[^"\\]|\\.)*)"|(\w+))', rest)
        if not m_schema:
            continue
        if m_schema.group(1) is not None:
            schema_of[m_id.group(1)] = m_schema.group(1)       # 原始字符串：不用解转义
        elif m_schema.group(2) is not None:
            schema_of[m_id.group(1)] = unescape_c(m_schema.group(2))
        else:
            schema_of[m_id.group(1)] = const_schema.get(m_schema.group(3), "")
    seq_needs_arg = []
    for s in km.get("sequences", []):
        spec = s.get("command", "")
        name, _, inline = spec.partition(":")
        if inline or not name:
            continue
        cid = to_cmd_id(name)
        if cid is None:
            continue          # 认不出的名字上面已经报过
        schema_txt = schema_of.get(cid) or ""
        if '"required"' not in schema_txt:
            continue
        m_req = re.search(r'"required"\s*:\s*\[(.*?)\]', schema_txt)
        if not m_req or not m_req.group(1).strip():
            continue          # required 是空数组：没有必填参数
        seq_needs_arg.append(f"{s.get('seq')}→{name}({m_req.group(1).replace(chr(34), '')})")
    if seq_needs_arg:
        errors.append(
            "序列绑了必填参数命令却没有内联参数（按下去必然失败，或静默改值）："
            + ", ".join(sorted(seq_needs_arg))
            + "。修法：给序列补内联参数（如 \"view.sort:2\"），或删掉这条序列"
              "（带参命令的正确入口是命令面板，它会按 schema 追问）")

    # ---- 规则 C1：命令 id 稳定（docs/NAMING.md §4.3）----
    #
    # 为什么需要基线：`CmdId` 是 enum class，没有显式赋值，所以值 = 声明顺序。
    # 现在没有任何地方持久化这个整数（审计记 name、keymap 记 name），但只要将来
    # 有人在中间插一条枚举，后面所有 id 就会位移 —— 那时如果已有外部脚本/AI 客户端
    # 按 id 调用，就会"调 A 执行 B"。这类 bug 在生产里表现为"偶发执行了错误的命令"，
    # 极难排查。所以现在就把顺序锁住：插新命令只能加在末尾。
    #
    # 记的不是数值而是**顺序**，因为顺序才是真正的契约（显式赋值后数值可任意）。
    baseline = ROOT / "data" / "commands-baseline.json"
    cur = {
        # 顺序 = **枚举声明顺序**，不是表行顺序。
        # 这两者不是一回事：表行顺序只影响菜单分组展示（可以随分组调整挪动），
        # 而 CmdId 的数值 = 枚举声明顺序。之前的实现误用表顺序，导致既会误报
        # （挪表行）又漏报（真改了枚举顺序但表里恰好没动）—— 实测两者当前确实不同：
        # 枚举 @69 是 CopyToFolder，表 @69 是 FileFocusTop。
        "ids": EnumOrder(),
        "legacyNames": sorted({a[0] for a in aliases}),   # 规则 C2：只增不减
    }
    if "--update-baseline" in sys.argv:
        baseline.write_text(json.dumps(cur, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
        print(f"已写入基线 {baseline.relative_to(ROOT)}"
              f"（{len(cur['ids'])} 个 id / {len(cur['legacyNames'])} 个旧名）")
        return 0

    if baseline.exists():
        old = json.loads(baseline.read_text(encoding="utf-8"))
        old_ids, new_ids = old.get("ids", []), cur["ids"]

        # C1a：旧序列必须是新序列的**前缀**。
        #
        # 不能写成"commons 顺序不变"—— 那样中间插入会漏报：
        #   old = [A,B,C]，new = [A,X,B,C] → common=[A,B,C]==old，看起来"顺序没变"，
        #   但 X 插在位置 1，B 从 1 变 2、C 从 2 变 3，全部位移。
        # 而"中间插入"正是 C1 要防的最危险情况。判据必须是前缀关系：
        #   [A,B,C] vs [A,B,C,D] → 前缀，放行（末尾追加）
        #   [A,B,C] vs [A,X,B,C] → 非前缀，报错（中间插入）
        if new_ids[:len(old_ids)] != old_ids:
            # 指出第一处不同，方便定位
            diff = next((i for i, (a, b) in enumerate(zip(old_ids, new_ids)) if a != b),
                        min(len(old_ids), len(new_ids)))
            errors.append(f"CmdId 顺序被改动：枚举第 {diff} 项起与基线不符"
                          f"（基线 {old_ids[diff] if diff < len(old_ids) else '<end>'}，"
                          f"现为 {new_ids[diff] if diff < len(new_ids) else '<end>'}）。"
                          "新增命令只能加在枚举末尾")

        # C1b：旧 id 不能删（删了后续 id 全部位移）
        missing = [x for x in old_ids if x not in set(new_ids)]
        if missing:
            errors.append("CmdId 被删除（应保留枚举项并标废弃，否则后续 id 全部位移）：" +
                          ", ".join(missing))

        # C2：旧名只增不减（VS Code legacyToolReferenceFullNames 的规矩）
        lost = [x for x in old["legacyNames"] if x not in set(cur["legacyNames"])]
        if lost:
            errors.append("legacy 名被删除（用户已有 keymap.json 会失效）：" + ", ".join(lost))
    else:
        print(f"说明  尚无基线 {baseline.relative_to(ROOT)}；"
              f"跑 python tools/check_commands.py --update-baseline 建立")

    # keymap.gen.h 是 data/keymap.json 的生成物（C++ 侧兜底默认表）。
    # 只改 json 不重新生成，就会出现"配置文件读得到新默认键、兜底仍是旧键"的静默不一致。
    gen_src = ROOT / "src" / "keymap.gen.h"
    if gen_src.exists():
        gen = strip_comments(gen_src.read_text(encoding="utf-8"))
        gen_hk = re.findall(r'\{L"([^"]*)",\s*L"([^"]*)",\s*L"([^"]*)"\}', gen)
        gen_sq = re.findall(r'\{L"([^"]*)",\s*L"([^"]*)"\}', gen)
        want_hk = [(h.get("setting", ""), h.get("command", ""), h.get("key", ""))
                   for h in km.get("hotkeys", [])]
        want_sq = [(s.get("seq", ""), s.get("command", "")) for s in km.get("sequences", [])]
        if gen_hk != want_hk or gen_sq != want_sq:
            errors.append("src/keymap.gen.h 与 data/keymap.json 不一致（改完 json 没重新生成）："
                          "跑 python tools/gen_keymap.py")

    # ---- 快捷键冲突（同一个 chord 被绑了两次）----
    #
    # 【为什么必须查】运行时 InputRouter::BuildBindings 的 add() 是"先到先得"：
    # 同一个 chord 第二次出现就被**静默丢弃**（src/input_router.cpp 的
    # `for (const auto& b : chords_) if (b.chord == c) return;`）。
    # 于是重复绑定的后果是：后一条命令**永远按不出来**，而且**没有任何提示** ——
    # 用户只看到"按了没反应"，排查时根本想不到是冲突。
    #
    # 历史上真出过：`Ctrl+Shift+A` 同时绑了 TabSwitcher 与 CopyToFolder
    # （2026-11-21 修掉，见 docs/COMPETITIVE-GAP-2026-11-21.md E6）。
    # 修掉的是**那一处**，不是**那一类** —— 所以这里补上机器能查的那一半。
    #
    # 【冲突源有三处，缺一不可】
    #   ① data/keymap.json 的 hotkeys（key 为空 → 落到 ②）
    #   ② src/keymap.cpp 的 kSysKeys（系统原生键兜底）
    #   ③ src/input_router.cpp 的 kExtra（应用内补充分配）
    # 只查 ① 会漏掉 ①② 撞车，也会漏掉 ①③ 撞车。
    #
    # 【口径必须与运行时一致】parse_chord 是 src/chord.cpp 的 chord::Parse 的
    # Python 复刻（同样的别名表、同样的修饰键归一）。口径不一致的检查等于没有：
    # 要么漏报真冲突，要么报一堆假冲突。
    chord_conflicts = find_chord_conflicts(km)
    if chord_conflicts:
        errors.append("快捷键冲突（同一组合被绑了两次，后一条永远按不出来）：" +
                      "; ".join(chord_conflicts))

    # ---- 序列重复 ----
    # 序列表里同一个 seq 出现两次 = 后一条永远触发不了（SequenceMachine::SetEntries
    # 是 add() 语义，见 src/sequence.cpp）。前缀关系（`x l` 与 `x l a`）**不算错**：
    # 引擎会等超时再提交短的那个（src/input_router.cpp 的 kSeqTimer → TakePendingExact），
    # 所以那种只在输出里提示，不报错。
    seq_seen = {}
    dup_seqs = []
    for s in km.get("sequences", []):
        q = (s.get("seq", "") or "").lower()
        if not q:
            continue
        if q in seq_seen:
            dup_seqs.append(f"{q}（{seq_seen[q]} 与 {s.get('command','')}）")
        else:
            seq_seen[q] = s.get("command", "")
    if dup_seqs:
        errors.append("序列重复（同一个序列被绑了两次，后一条永远触发不了）：" +
                      "; ".join(dup_seqs))

    print(f"命令表 {len(rows)} 条，已实现 {len(implemented)}，处理器 {len(handlers)}，"
          f"keymap 引用 {len(set(specs))} 条")
    # 占位是无害的"已知欠债"，不是错误，但要让它在输出里可见 —— 否则 PENDING
    # 会变成一张只增不减的免死金牌，没人再回头看哪条其实早该实现或该删键位。
    if pending_hit:
        print(f"注意  {len(pending_hit)} 条键位为占位（命令未实现，已在 PENDING 登记）：" +
              ", ".join(sorted(f"{s}→{n}" for s, n in pending_hit)))
    if errors:
        for e in errors:
            print("FAIL " + e)
        return 1
    print("OK 命令目录 / keymap / 处理器三方一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
