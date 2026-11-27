#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 package_release.py 的 FACTORY_SETTINGS 与源码出厂默认值一致。

为什么需要：发布包里的 data/settings.json 是"出厂自带、拿来能用"的配置，
由 package_release.py 的 FACTORY_SETTINGS 常量直接生成。它一旦与 config.h 的
Settings 默认值漂移，发布包就会悄悄带错值，而 verify_release.py 只抽查几个
字段，抓不到。本脚本以源码为真相源做全量逐字段比对。

真相源：
  - src/config_io.cpp       Config::Save() 的 root.Set 清单 = settings.json 字段全集
  - src/config.h            struct Settings 的成员默认值
  - src/layout_limits.h     kNavPaneWidthDefault / kAiPanelWidthDefault
  - src/view_mode_policy.h  struct Prefs 的 systemFolderMode / reapplyOnNavigate
  - scripts/package_release.py  FACTORY_SETTINGS（被检查对象）

用法：python tools/check_factory_settings.py
退出码：0 = 全一致；1 = 有漂移。
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
PKG = ROOT / "scripts" / "package_release.py"


def read(path):
    return path.read_text(encoding="utf-8")


def struct_body(text, name):
    """取 `struct Name { ... };` 的花括号内容（按大括号配平，不靠行号）。"""
    m = re.search(r"\bstruct\s+" + re.escape(name) + r"\s*\{", text)
    if not m:
        return ""
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[start:i - 1] if depth == 0 else ""


def save_keys():
    """Config::Save() 里所有 root.Set(L"键", ...s_.成员...) 的映射。"""
    text = read(SRC / "config_io.cpp")
    out = []
    pat = re.compile(r'root\.Set\(L"([^"]+)",.*?s_\.([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)?)')
    for m in pat.finditer(text):
        out.append((m.group(1), m.group(2)))
    return out


def member_defaults():
    """Settings / Prefs 结构体成员 → 默认值表达式（`=` 形式）。"""
    out = {}
    for fname, struct in (("config.h", "Settings"), ("view_mode_policy.h", "Prefs")):
        body = struct_body(read(SRC / fname), struct)
        # 逐行取 `Type name = expr;`；跳过花括号初始化（如 statusSegs{...}）。
        for line in body.splitlines():
            m = re.match(r"\s*[A-Za-z_][\w:<>, ]*?\s+([A-Za-z_]\w*)\s*=\s*([^;]+);", line)
            if m:
                out.setdefault(m.group(1), m.group(2).strip())
    return out


def constant(path, name):
    m = re.search(r"constexpr\s+int\s+" + re.escape(name) + r"\s*=\s*(-?\d+)\s*;", read(path))
    return int(m.group(1)) if m else None


FAIL = object()  # 解析失败哨兵：不能用 False，因为它本身是合法默认值


def eval_default(expr, defaults, consts):
    """把 C++ 默认值表达式转成 Python 值；无法解析返回 FAIL。"""
    e = expr.strip()
    if e == "true":
        return True
    if e == "false":
        return False
    if re.fullmatch(r"-?\d+", e):
        return int(e)
    m = re.fullmatch(r'L"([^"]*)"', e)
    if m:
        return m.group(1)
    if e in consts:
        return consts[e]
    # 嵌套成员：s_.viewPrefs.systemFolderMode → 取末段
    tail = e.split(".")[-1]
    if tail in defaults:
        return eval_default(defaults[tail], defaults, consts)
    return FAIL


def factory_settings():
    text = read(PKG)
    m = re.search(r'FACTORY_SETTINGS\s*=\s*"""(.*?)"""', text, re.S)
    if not m:
        return None
    return json.loads(m.group(1))


def main():
    keys = save_keys()
    defaults = member_defaults()
    consts = {
        "kNavPaneWidthDefault": constant(SRC / "layout_limits.h", "kNavPaneWidthDefault"),
        "kAiPanelWidthDefault": constant(SRC / "layout_limits.h", "kAiPanelWidthDefault"),
    }
    factory = factory_settings()

    errs = []
    if factory is None:
        print("[FAIL] package_release.py 里找不到 FACTORY_SETTINGS 常量")
        return 1

    save_names = [k for k, _ in keys]
    # 1. 键集合完全一致（不缺、不多）
    missing = [k for k in save_names if k not in factory]
    extra = [k for k in factory if k not in save_names]
    if missing:
        errs.append("出厂配置缺少字段: " + ", ".join(missing))
    if extra:
        errs.append("出厂配置多出字段（Save 不写）: " + ", ".join(extra))

    # 2. 逐字段比对值
    bad = []
    unresolved = []
    for key, member in keys:
        if key not in factory:
            continue
        tail = member.split(".")[-1]
        # 空容器：vector 成员无 `= expr`，用末段名特判
        if tail in ("navPaneRoots", "rules"):
            want = []
        else:
            expr = defaults.get(tail)
            if expr is None:
                unresolved.append("%s (%s)" % (key, member))
                continue
            want = eval_default(expr, defaults, consts)
        if want is FAIL:
            unresolved.append("%s (%s = %s)" % (key, member, expr))
            continue
        if factory[key] != want:
            bad.append("%s: 出厂=%r 源码默认=%r" % (key, factory[key], want))

    if unresolved:
        errs.append("无法解析源码默认值的字段: " + ", ".join(unresolved))
    if bad:
        errs.append("出厂值与源码默认值不一致:\n    " + "\n    ".join(bad))

    if errs:
        print("[FAIL] FACTORY_SETTINGS 与源码出厂默认值不同步：")
        for e in errs:
            print("  - " + e)
        return 1

    print("[OK] FACTORY_SETTINGS 与源码出厂默认值一致：%d 个字段逐项匹配" % len(keys))
    return 0


if __name__ == "__main__":
    sys.exit(main())
