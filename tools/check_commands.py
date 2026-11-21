#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令目录静态审计（CI / 提交前跑一次，无窗口依赖）。

检查四件事，任何一条不满足都说明"键位/面板点得到但没实现"：
  1. data/keymap.json 里出现的每条命令（hotkeys + sequences）都在 src/commands.cpp 的表里；
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
TABLE_SRC = ROOT / "src" / "commands.cpp"
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
    # 批量重命名建议直接接 PowerRename，不自建规则编辑器（OPERATION-MODEL §7）。
    "hotkeyBatchRename": "BatchRename 待实现（接 PowerRename）",
    # 搜索框与 Ctrl+F 已于 2026-11-21 落地（NavBar::FocusSearch + search-ms）。
    # 工具集三条已于 2026-11-21 实现：DupFinder / FileHash / FolderCompare
    # （file_hash.cpp + file_tools.cpp，预算内同步扫描 + 只读报告窗口）。
    # 这里原先的理由写着"需 helper 进程 + IPC"，实现时发现不必要：
    # 预算是显式的（timedOut/complete 如实上报），单文件哈希本来就必须读完整文件，
    # 用 helper 换来的只是"扫描时界面不卡"，代价是一整套 IPC 与生命周期管理。
    # 代价是预算耗尽会少扫一部分 —— 报告里写清楚了，比假死或假数字好。
    # AI 面板：接口层（ai_api / ai_server）已通，缺窗口内对话 UI 与历史（ROADMAP §5 P4）。
    "hotkeyAiTogglePanel": "AiTogglePanel 待实现（缺面板 UI，接口层已就绪）",
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
    """按声明顺序读出 commands.h 里 `enum class CmdId` 的全部标识符。

    只认 word 形态的 token，跳过显式赋值（`None = 0`）与注释。
    这是 CmdId 数值的真实来源 —— 与 commands.cpp 表行的顺序无关。
    """
    src = (ROOT / "src" / "commands.h").read_text(encoding="utf-8")
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


def main():
    table_src = strip_comments(TABLE_SRC.read_text(encoding="utf-8"))
    # 命令表条目现在带结构化元数据（Effects{...}/argsSchema/aiExposed/needsSelection/
    # Requirements{...}），argsSchema 是含大量 '}' 的 JSON 字符串、Effects 本身也是
    # 花括号 —— 跨行/跨括号的整条匹配极易失配。
    #
    # 改为**按行**匹配：每个条目在 commands.cpp 里固定占一行，只取前 6 个字段
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
        print("FAIL 没能从 commands.cpp 解析出命令表")
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
