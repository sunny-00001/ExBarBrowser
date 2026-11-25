#!/usr/bin/env python3
# E2E 闭环验收：用 P0–P3 的新范式跑"书签移到末尾"，证明全程无需手工胶水。
#
# 对比改进前（手写 ebb_bmk_probe.py），现在只需：
#   · 程序用 --ai 启动（P0 通道自举，不碰 settings.json）
#   · 一条意图命令完成发现→执行→验证（P3），自带 verdict（P2）
#   · 无手写 ctypes、无临时改设置、无另写回读脚本
#
# 本脚本假设程序已用 --ai 启动、通道就绪。它验证：
#   1. intent.bookmark.move-to-end 返回 verdict=true，且验收 passed
#   2. 回读确认目标确实在顶层末位
#   3. 磁盘 bookmarks.html 末条是目标（落盘）
#   4. 审计记录了 bmk.move（logs/audit.log）
#   5. settings.json 的 aiEnabled 未被改动（P0 核心承诺）
import io
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from ebbctl import Client
from intents import run as run_intent

PROFILE = ROOT                       # profile 根；Client 自己去 runtime\ 找发现文件
DATA = os.path.join(ROOT, "data")    # 低频配置仍平铺在 data\
LOGS = os.path.join(ROOT, "logs")    # 审计已迁到 logs\
fails = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        fails.append(what)


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "坚果云"
    client = Client(PROFILE)

    # 记录 aiEnabled 原值（P0：意图执行不应改它）
    sp = os.path.join(DATA, "settings.json")
    before_cfg = io.open(sp, encoding="utf-8-sig").read()
    before_enabled = '"aiEnabled": true' in before_cfg

    print("意图：把 %r 移到顶层最后" % target)
    r = run_intent(client, "bookmark.move-to-end", {"name": target})
    print(json.dumps(r, ensure_ascii=False, indent=2))

    check(r.get("verdict") is True, "意图 verdict=true")
    check(r.get("verify", {}).get("passed") is True, "内置验收 passed")
    check(all(s.get("verdict") for s in r.get("steps", [])), "每步 verdict 都为真")

    # 独立回读（意在证明"即便不信任意图自报，数据也对"）
    after = client.call("bmk.list", {})
    entries = after.get("structuredContent", {}).get("entries", [])
    roots = [e for e in entries if not e.get("parentId")]
    check(bool(roots) and roots[-1].get("name") == target,
          "独立回读：%r 在顶层末位" % target)

    # 磁盘落盘
    text = io.open(os.path.join(DATA, "bookmarks.html"), encoding="utf-8-sig").read()
    last = [l for l in text.splitlines() if "EXBAR_ID=" in l][-1]
    check(target in last, "磁盘末条是目标（已落盘）")

    # 审计
    audit = io.open(os.path.join(LOGS, "audit.log"), encoding="utf-8", errors="replace").read()
    check("bmk.move" in audit[-4000:], "审计记录了 bmk.move")

    # P0 承诺：执行意图不改 aiEnabled
    after_cfg = io.open(sp, encoding="utf-8-sig").read()
    after_enabled = '"aiEnabled": true' in after_cfg
    check(after_enabled == before_enabled, "settings.json 的 aiEnabled 未被意图改动")

    print("\n结论：", "全部通过" if not fails else ("失败 %d 项：%s" % (len(fails), fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
