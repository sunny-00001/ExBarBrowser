#!/usr/bin/env python3
# 意图目录：把"用户想做的事"声明成已有命令的编排配方。
#
# 为什么放客户端层（而不是新增 intent.run 命令）：
#   · 意图本质是命令编排，每一步都是普通 tools/call，没有旁路、不开新通道；
#   · 服务端 Dispatch 的 lastBlockReason_ 是可重入敏感的单槽，handler 嵌套调用
#     会踩掉外层状态 —— 编排放在客户端就没有这个问题；
#   · ebbctl 已是统一客户端（P1），配方表放这里是单一真源：加意图 = 加一行数据。
#
# 配方语法（小而够用，不做通用表达式引擎）：
#   参数绑定：
#     "$p.NAME"                    意图入参
#     "$s.STEP.findId(name=EXP)"   从前一步 entries 里按 name 找 id（EXP 可再含 $p/$s）
#     "$s.STEP.<field>"            前一步结果里的标量字段
#   验收（verify）：
#     {"last_name_of": "STEP", "equals": "$p.NAME"}   STEP 的 entries 末项 name 等于期望
#
# 每个配方执行后返回 {ok, verdict, intent, steps:[{name,ok,verdict}], verify}。
# 全程只走 Client.call()，因此天然带 P2 的 verdict。
import re
import sys

from ebbctl import Client

# ---- 声明式意图表：加一行 = 多一个意图，不改执行器 ----
INTENTS = {
    "bookmark.move-to-end": {
        "desc": "把指定书签移到顶层最后一位",
        "params": ["name"],
        "steps": [
            {"as": "before", "call": "bmk.list", "args": {}},
            # 幂等：已经在末位就别再 move（MoveTo 对原地移动返回 false）。
            {"as": "move", "call": "bmk.move",
             "args": {"id": "$s.before.findId(name=$p.name)"},
             "skip_if": {"last_name_of": "before", "equals": "$p.name"}},
            {"as": "after", "call": "bmk.list", "args": {}},
        ],
        "verify": {"last_name_of": "after", "equals": "$p.name"},
    },
    "bookmark.move-to-top": {
        "desc": "把指定书签移到顶层第一位",
        "params": ["name"],
        "steps": [
            {"as": "before", "call": "bmk.list", "args": {}},
            {"as": "move", "call": "bmk.move",
             "args": {"id": "$s.before.findId(name=$p.name)", "toIndex": 0},
             "skip_if": {"first_name_of": "before", "equals": "$p.name"}},
            {"as": "after", "call": "bmk.list", "args": {}},
        ],
        "verify": {"first_name_of": "after", "equals": "$p.name"},
    },
}

_TPL = re.compile(r"^\$s\.([A-Za-z0-9_]+)\.findId\(name=(.+)\)$")
_PARAM = re.compile(r"^\$p\.([A-Za-z0-9_]+)$")


def resolve(expr, params, results):
    """解析一个绑定表达式；非字符串原样返回。"""
    if not isinstance(expr, str):
        return expr
    m = _TPL.match(expr)
    if m:
        step, name_expr = m.group(1), m.group(2)
        name = resolve(name_expr, params, results)
        entries = extract_entries(results.get(step))
        for it in entries:
            if str(it.get("name", "")) == str(name):
                return it.get("id")
        raise KeyError("在步骤 %s 里找不到名为 %r 的书签" % (step, name))
    m = _PARAM.match(expr)
    if m:
        key = m.group(1)
        if key not in params:
            raise KeyError("缺少意图参数 %s" % key)
        return params[key]
    return expr


def _payload(r):
    return r.get("structuredContent") or r.get("result") or r


def extract_entries(result):
    p = _payload(result or {})
    return p.get("entries") or p.get("items") or []


def check_position(cond, params, results):
    """位置型判据：{last_name_of|first_name_of: STEP, equals: EXP} → bool。"""
    step = cond.get("last_name_of") or cond.get("first_name_of")
    entries = extract_entries(results.get(step))
    expected = str(resolve(cond["equals"], params, results))
    if not entries:
        return False
    actual = entries[-1] if "last_name_of" in cond else entries[0]
    return str(actual.get("name", "")) == expected


def run(client, intent_name, params):
    recipe = INTENTS.get(intent_name)
    if recipe is None:
        return {"ok": False, "verdict": False, "intent": intent_name,
                "error": "未知意图；可用：" + ", ".join(sorted(INTENTS))}

    missing = [k for k in recipe["params"] if k not in params]
    if missing:
        return {"ok": False, "verdict": False, "intent": intent_name,
                "error": "缺少参数：" + ", ".join(missing)}

    results = {}
    steps = []
    for st in recipe["steps"]:
        # 幂等：目标状态已成立就跳过这一步（如书签已在末位，MoveTo 原地返回 false）。
        if "skip_if" in st and check_position(st["skip_if"], params, results):
            steps.append({"as": st["as"], "call": st["call"], "skipped": True,
                          "ok": True, "verdict": True, "error": None})
            continue
        args = {k: resolve(v, params, results) for k, v in st["args"].items()}
        r = client.call(st["call"], args)
        results[st["as"]] = r
        steps.append({"as": st["as"], "call": st["call"], "args": args,
                      "ok": r.get("ok"), "verdict": r.get("verdict", r.get("ok")),
                      "error": r.get("error")})
        # 任一步失败/效果不对，立即停 —— 不带着错误状态继续往下做。
        if not r.get("ok") or r.get("verdict", r.get("ok")) is False:
            return {"ok": False, "verdict": False, "intent": intent_name,
                    "steps": steps, "error": "步骤 %s 未成功" % st["as"]}

    # 验收：用最后一步的结果确认意图真的达成（不依赖 bmk.move 自己的 ok）
    verify = recipe["verify"]
    passed = check_position(verify, params, results)
    step = verify.get("last_name_of") or verify.get("first_name_of")
    entries = extract_entries(results.get(step))
    expected = str(resolve(verify["equals"], params, results))
    actual = (entries[-1] if "last_name_of" in verify else entries[0]) if entries else {}
    return {"ok": True, "verdict": passed, "intent": intent_name,
            "steps": steps,
            "verify": {"expected": expected, "actual": actual.get("name"), "passed": passed}}


def main(argv=None):
    import argparse
    import json
    ap = argparse.ArgumentParser(description="意图目录：一次调用完成多步操作+验收")
    ap.add_argument("--data-dir")
    ap.add_argument("intent", help="意图名（如 bookmark.move-to-end）；list 列出全部")
    ap.add_argument("params", nargs="*", help="key=value，如 name=坚果云")
    a = ap.parse_args(argv)

    if a.intent == "list":
        for name, r in sorted(INTENTS.items()):
            print("%-28s %s（参数：%s）" % (name, r["desc"], ", ".join(r["params"])))
        return 0

    params = {}
    for kv in a.params:
        if "=" not in kv:
            ap.error("参数要写成 key=value：%s" % kv)
        k, v = kv.split("=", 1)
        params[k] = v

    try:
        r = run(Client(a.data_dir), a.intent, params)
    except KeyError as ex:
        r = {"ok": False, "verdict": False, "error": str(ex)}
    print(json.dumps(r, ensure_ascii=False, indent=2))
    return 0 if r.get("verdict") else 1


if __name__ == "__main__":
    sys.exit(main())
