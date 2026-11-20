#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_keymap.py —— 键位默认表「数据 -> 代码」生成器（ExBarBrowser）

单一真相：`data/keymap.json`（与 ExBar mod 的 data/defaults/keymap.json 同格式）。
本脚本由它生成 `src/keymap.gen.h`，供 keymap.cpp 在「配置文件缺失/损坏」时兜底。

为什么不再手抄一份到 C++：
    src/keymap.cpp 原来的 kDefaultHotkeys[] / kDefaultSeqs[] 是 json 的逐条副本
    （70 + 38 行）。改默认键要同时改 json 与 C++ 两处，漏一处就出现"装了 mod 的
    默认键、但本程序兜底还是旧键"的静默不一致。生成后只有一处真相。

用法：
    python tools/gen_keymap.py            # 由 json 重新生成 src/keymap.gen.h
    python tools/gen_keymap.py --check    # 只校验：生成物是否与 json 一致（CI 用）
退出码：0 = 一致/已写入；1 = --check 且过期；2 = 输入错误。
"""
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYMAP = os.path.join(ROOT, "data", "keymap.json")
OUT = os.path.join(ROOT, "src", "keymap.gen.h")

BANNER = """// 由 tools/gen_keymap.py 从 data/keymap.json 生成 —— 不要手改本文件。
// 改默认键：编辑 data/keymap.json，再跑 python tools/gen_keymap.py。
// --check 会在 CI 里校验两者一致（过期即失败）。
"""


def cstr(s):
    """C++ 宽字符串字面量：转义反斜杠与引号。"""
    return 'L"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(data):
    hot = data.get("hotkeys", [])
    seq = data.get("sequences", [])
    lines = [BANNER, "#pragma once", "", "namespace keymap_gen {", ""]

    lines.append("// setting / command / 默认组合键（空串 = 用系统原生键，不占组合键）")
    lines.append("struct DefHotkey {")
    lines.append("    const wchar_t* setting;")
    lines.append("    const wchar_t* command;")
    lines.append("    const wchar_t* key;")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr DefHotkey kHotkeys[] = {{")
    for h in hot:
        lines.append("    {%s, %s, %s}," % (cstr(h["setting"]), cstr(h["command"]),
                                            cstr(h.get("key", ""))))
    lines.append("};")
    lines.append("")

    lines.append("struct DefSeq {")
    lines.append("    const wchar_t* seq;")
    lines.append("    const wchar_t* command;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr DefSeq kSeqs[] = {")
    for s in seq:
        lines.append("    {%s, %s}," % (cstr(s["seq"]), cstr(s["command"])))
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace keymap_gen")
    lines.append("")
    return "\n".join(lines)


def load():
    with io.open(KEYMAP, "r", encoding="utf-8-sig") as f:
        data = json.load(f)
    if not isinstance(data.get("hotkeys"), list) or not isinstance(data.get("sequences"), list):
        raise SystemExit("data/keymap.json 缺少 hotkeys/sequences 数组")
    return data


def main():
    check = "--check" in sys.argv[1:]
    try:
        data = load()
    except SystemExit:
        raise
    except Exception as e:
        print("读取 data/keymap.json 失败:", e)
        return 2

    text = render(data)
    old = None
    if os.path.exists(OUT):
        with io.open(OUT, "r", encoding="utf-8") as f:
            old = f.read()

    same = (old == text)
    if check:
        if same:
            print(f"OK keymap.gen.h 与 data/keymap.json 一致"
                  f"（{len(data['hotkeys'])} 热键 / {len(data['sequences'])} 序列）")
            return 0
        print("FAIL src/keymap.gen.h 已过期：改过 data/keymap.json 但没重新生成。")
        print("     修：python tools/gen_keymap.py")
        return 1

    if same:
        print(f"OK 无变化（{len(data['hotkeys'])} 热键 / {len(data['sequences'])} 序列）")
        return 0
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"写入 src/keymap.gen.h（{len(data['hotkeys'])} 热键 / "
          f"{len(data['sequences'])} 序列）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
