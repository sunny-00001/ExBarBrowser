#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令目录静态审计（CI / 提交前跑一次，无窗口依赖）。

检查三件事，任何一条不满足都说明"键位/面板点得到但没实现"：
  1. data/keymap.json 里出现的每条命令（hotkeys + sequences）都在 src/commands.cpp 的表里；
  2. 表里 implemented=true 的命令，在 src/app_commands.cpp 里都有 reg(CmdId::X) 处理器；
  3. 表里 implemented=false 的命令不能有处理器（否则是"实现了却标成未实现"）。

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


def strip_comments(text):
    """去掉整行/行尾 // 注释，避免文档里的示例被当成真实注册。"""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


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
        "ids": [r[0] for r in rows],                      # 顺序 = CmdId 的值序
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

        # C1a：已有 id 的相对顺序不能变（允许在末尾追加，不允许中间插入/删除/重排）
        common = [x for x in new_ids if x in set(old_ids)]
        missing = [x for x in old_ids if x not in set(new_ids)]
        if common != old_ids:
            errors.append("CmdId 顺序被改动（会位移所有后续 id 的数值）："
                          "新增命令只能加在枚举末尾")
        # C1b：删掉的 id 必须保留占位（规则 C1 的原文："废弃保留占位"）
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
    if errors:
        for e in errors:
            print("FAIL " + e)
        return 1
    print("OK 命令目录 / keymap / 处理器三方一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
