#!/usr/bin/env python3
# 跨会话经验沉淀（C5）与污染防护（C5b）。
#
# 为什么不能"把成功经验追加到一个文件里"就算完：
#   · EVOMAL（arXiv:2608.25776）：恶意技能自发复制 4.9–9.0 倍，删掉植入源后
#     第 5 轮仍有 68% 攻击成功率 —— 沉淀层本身就是传播通道；
#   · arXiv:2609.04170：作弊方法经共享知识库传开，还会出现"吹哨人"审计；
#   · arXiv:2608.05810：技能池超临界规模反而拉低表现，污染链结构上不可逆。
# 所以入库必须先验证、可疑必须隔离、容量必须有硬上限、每次变更必须进
# 不可篡改的 ledger。这四条是**机制**，不是注释里写一句"注意安全"。
#
# 三条流分开：
#   experience.jsonl          活跃经验池（≤3 条，Reflexion 的容量口径）
#   experience-quarantine.jsonl  可疑条目：留证但**永不参与检索**
#   experience-ledger.jsonl   追加流水：add/upvote/downvote/edit/evict 全记
#
# 本模块不碰窗口、不发输入消息（R21），纯文件 + 调用方注入的验证回调。
import datetime
import hashlib
import json
import os
import re


ACTIVE = "experience.jsonl"
QUARANTINE = "experience-quarantine.jsonl"
LEDGER = "experience-ledger.jsonl"

MAX_ACTIVE = 3          # Reflexion：情景记忆上限 3 条
MAX_LEDGER = 2000       # 流水也要有上限，否则本身就是无界增长
KINDS = ("success", "failure", "correction")

# 命中即隔离。宁可误隔离一条正常经验，也不要让"教你绕过检查"进池子 ——
# 前者损失一条经验，后者会把作弊方法复制到后续每一次运行。
_SUSPICIOUS = (
    r"ignore\s+(all\s+)?previous\s+instructions",
    r"忽略.{0,8}(之前|上面|前面).{0,8}(指令|要求|规则)",
    r"(复制|传播|扩散|分享).{0,8}(自己|本条|这条|给其他|到其他)",
    r"\breplicat(e|ion|ing)\b",
    r"\bself[- ]?propagat",
    r"(绕过|关闭|禁用|跳过).{0,12}(检查|校验|验证|审计|eval)",
    r"(disable|bypass|skip).{0,20}(check|valid|audit|test|eval)",
    r"(不要|无需|别).{0,12}(告诉|报告|记录|审计)",
    r"(修改|改写|删除|覆盖).{0,12}(tools/|tests/|build_fast|eval-surface|check_ai_meta)",
    r"(改成|设为).{0,8}always\s*(true|allow)",
)
_SUSPICIOUS_RE = [re.compile(p, re.I) for p in _SUSPICIOUS]


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")


def paths(data_dir):
    return (os.path.join(data_dir, ACTIVE),
            os.path.join(data_dir, QUARANTINE),
            os.path.join(data_dir, LEDGER))


def read_jsonl(path):
    """逐行解析；坏行跳过但不静默 —— 调用方可以从返回的坏行数看出文件被写过。"""
    rows, bad = [], 0
    if not os.path.exists(path):
        return rows, bad
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                bad += 1
                continue
            if isinstance(obj, dict):
                rows.append(obj)
    return rows, bad


def append_jsonl(path, obj, cap=0):
    """追加一行；cap>0 时超出保留最后 cap 行（ledger 用，活跃池不用）。"""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    line = json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n"
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write(line)
    if cap and os.path.getsize(path) > cap * 4096:
        rows, _ = read_jsonl(path)
        keep = rows[-cap:]
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            for r in keep:
                f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")


def normalize(text):
    return re.sub(r"\s+", " ", (text or "").strip()).lower()


def entry_hash(entry):
    """去重键：同一条经验反复 add 应该 UPVOTE，不是复制出第二份。"""
    raw = normalize(entry.get("command", "")) + "|" + normalize(entry.get("insight", ""))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:16]


def suspicious_reasons(entry):
    """扫 insight 与 evidence —— 只扫 insight 会被"证据里夹带私货"绕过。"""
    hay = " ".join(str(entry.get(k, "")) for k in ("insight", "kind", "command"))
    ev = entry.get("evidence")
    if ev:
        hay += " " + json.dumps(ev, ensure_ascii=False)
    return [p.pattern for p in _SUSPICIOUS_RE if p.search(hay)]


def validate(entry):
    if not isinstance(entry, dict):
        return "entry_must_be_object"
    if entry.get("kind") not in KINDS:
        return "bad_kind"
    if not isinstance(entry.get("command"), str) or not entry["command"].strip():
        return "missing_command"
    insight = entry.get("insight")
    if not isinstance(insight, str) or len(insight.strip()) < 4:
        return "insight_too_short"
    if len(insight) > 2000:
        return "insight_too_long"
    return ""


def _ledger(data_dir, action, entry_id, reason, extra=None):
    rec = {"ts": now_iso(), "action": action, "id": entry_id, "reason": reason}
    if extra:
        rec.update(extra)
    append_jsonl(os.path.join(data_dir, LEDGER), rec, cap=MAX_LEDGER)


def _quarantine(data_dir, entry, reasons):
    rec = dict(entry)
    rec.update({"id": entry_hash(entry), "state": "quarantined",
                "quarantinedAt": now_iso(), "reasons": reasons})
    append_jsonl(os.path.join(data_dir, QUARANTINE), rec)
    _ledger(data_dir, "quarantine", rec["id"], "suspicious_or_unverified",
            {"reasons": reasons})
    return rec


def verify_against_events(data_dir, proof, command):
    """写命令不能靠自报成功：proof 必须指向 events.jsonl 里真实存在的一条。"""
    if not isinstance(proof, dict):
        return False, "proof_missing"
    seq = proof.get("seq")
    trace_id = proof.get("traceId")
    if not isinstance(seq, int) or not trace_id:
        return False, "proof_incomplete"
    events_path = os.path.join(data_dir, "events.jsonl")
    rows, _ = read_jsonl(events_path)
    for e in rows:
        if e.get("seq") != seq or e.get("traceId") != trace_id:
            continue
        data = e.get("data") or {}
        if data.get("command") != command or not data.get("ok"):
            continue
        if data.get("verdict") is False:
            continue
        return True, "event_verified"
    return False, "no_matching_success_event"


def add(data_dir, entry, verify_fn=None, max_active=MAX_ACTIVE):
    """ADD：先验后入。verify_fn(command, args) -> (bool, detail)，由调用方注入。
    不传 verify_fn 一律拒绝 —— "没验证"不能伪装成"验证通过"。"""
    err = validate(entry)
    if err:
        return {"ok": False, "reason": err}
    reasons = suspicious_reasons(entry)
    if reasons:
        q = _quarantine(data_dir, entry, reasons)
        return {"ok": False, "reason": "quarantined", "id": q["id"], "reasons": reasons}

    command = entry["command"]
    verified = False
    detail = ""
    if callable(verify_fn):
        verified, detail = verify_fn(command, entry.get("args") or {})
    if not verified:
        q = _quarantine(data_dir, entry, ["verification_failed:" + str(detail)])
        return {"ok": False, "reason": "verification_failed", "id": q["id"], "detail": detail}

    active_path, _, _ = paths(data_dir)
    active, bad = read_jsonl(active_path)
    eid = entry_hash(entry)
    for old in active:
        if old.get("id") == eid:
            old["score"] = int(old.get("score", 0)) + 1
            old["lastSeenAt"] = now_iso()
            with open(active_path, "w", encoding="utf-8", newline="\n") as f:
                for r in active:
                    f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
            _ledger(data_dir, "upvote", eid, "duplicate_add", {"detail": detail})
            return {"ok": True, "action": "upvote", "id": eid, "score": old["score"]}

    rec = dict(entry)
    rec.update({"id": eid, "state": "active", "score": 0,
                "createdAt": now_iso(), "verifiedBy": detail})
    active.append(rec)
    evicted = []
    while len(active) > max_active:
        # 淘汰：先低分，再老的。绝不"随机删" —— 淘汰理由要能进 ledger 复盘。
        active.sort(key=lambda r: (int(r.get("score", 0)), r.get("createdAt", "")))
        victim = active.pop(0)
        evicted.append(victim["id"])
        _ledger(data_dir, "evict", victim["id"], "capacity",
                {"maxActive": max_active, "replacement": eid})
    with open(active_path, "w", encoding="utf-8", newline="\n") as f:
        for r in active:
            f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
    _ledger(data_dir, "add", eid, "verified", {"detail": detail, "evicted": evicted})
    return {"ok": True, "action": "add", "id": eid, "active": len(active),
            "evicted": evicted, "corruptLines": bad}


def find(data_dir, entry_id):
    active_path, _, _ = paths(data_dir)
    active, _ = read_jsonl(active_path)
    for r in active:
        if r.get("id") == entry_id:
            return r, active
    return None, active


def vote(data_dir, entry_id, direction):
    """UPVOTE / DOWNVOTE：分数归零以下就淘汰，理由进 ledger。"""
    if direction not in ("up", "down"):
        return {"ok": False, "reason": "bad_direction"}
    rec, active = find(data_dir, entry_id)
    if rec is None:
        return {"ok": False, "reason": "not_found"}
    rec["score"] = int(rec.get("score", 0)) + (1 if direction == "up" else -1)
    action = "upvote" if direction == "up" else "downvote"
    evicted = []
    if rec["score"] < 0:
        active = [r for r in active if r.get("id") != entry_id]
        evicted.append(entry_id)
    active_path, _, _ = paths(data_dir)
    with open(active_path, "w", encoding="utf-8", newline="\n") as f:
        for r in active:
            f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
    _ledger(data_dir, action, entry_id, "explicit_vote", {"score": rec["score"]})
    for eid in evicted:
        _ledger(data_dir, "evict", eid, "negative_score")
    return {"ok": True, "action": action, "id": entry_id, "score": rec["score"],
            "evicted": evicted}


def edit(data_dir, entry_id, insight, verify_fn=None):
    """EDIT 必须重新过一遍可疑扫描与验证 —— 否则"先验证再入库"可以靠编辑绕过。"""
    rec, active = find(data_dir, entry_id)
    if rec is None:
        return {"ok": False, "reason": "not_found"}
    cand = dict(rec)
    cand["insight"] = insight
    reasons = suspicious_reasons(cand)
    if reasons:
        _quarantine(data_dir, cand, reasons)
        active = [r for r in active if r.get("id") != entry_id]
        active_path, _, _ = paths(data_dir)
        with open(active_path, "w", encoding="utf-8", newline="\n") as f:
            for r in active:
                f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
        return {"ok": False, "reason": "quarantined", "reasons": reasons}
    verified, detail = (verify_fn(rec["command"], rec.get("args") or {})
                        if callable(verify_fn) else (False, "no_verifier"))
    if not verified:
        return {"ok": False, "reason": "verification_failed", "detail": detail}
    rec["insight"] = insight
    rec["editedAt"] = now_iso()
    rec["verifiedBy"] = detail
    active_path, _, _ = paths(data_dir)
    with open(active_path, "w", encoding="utf-8", newline="\n") as f:
        for r in active:
            f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
    _ledger(data_dir, "edit", entry_id, "reverified", {"detail": detail})
    return {"ok": True, "action": "edit", "id": entry_id}


def list_entries(data_dir, include_quarantine=False):
    active_path, quarantine_path, ledger_path = paths(data_dir)
    active, bad_active = read_jsonl(active_path)
    q, bad_q = read_jsonl(quarantine_path)
    ledger, bad_ledger = read_jsonl(ledger_path)
    out = {"active": active, "activeCount": len(active), "maxActive": MAX_ACTIVE,
           "quarantineCount": len(q), "ledgerCount": len(ledger),
           "corruptLines": {"active": bad_active, "quarantine": bad_q, "ledger": bad_ledger}}
    if include_quarantine:
        out["quarantine"] = q
        out["ledger"] = ledger
    return out


# ---------------------------------------------------------------- B7 统计

UNDO_COMMANDS = {"file.undo", "bmk.undo", "dev.rollback"}


def _is_write(cmd):
    # 事件/审计里都没有 Effects 位，只能按名字粗分；这里只用于分母，不用于决策。
    return not cmd.startswith(("state", "view.", "tab.list", "bmk.list", "search.",
                               "config.get", "config.list", "tool.", "dev.job"))


def _count_tool(table, cmd, bad, code):
    slot = table.setdefault(cmd, {"total": 0, "errors": 0, "errorCodes": {}})
    slot["total"] += 1
    if bad:
        slot["errors"] += 1
        key = code or "unknown"
        slot["errorCodes"][key] = slot["errorCodes"].get(key, 0) + 1


def _reads(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip():
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(obj, dict):
                yield obj


def stats(data_dir):
    """B7：每工具错误率 / 每任务轮数 / 人工改写率。
    全部从落盘事实算，不读内存、不靠自报。人工改写率的分母是 AI 执行过的
    写命令数，分子是同目录审计里人工发起的撤销/回滚数 —— 这是当前代码能
    给出的最接近"人改了 AI 的产出"的可观测信号。等 A10 的 edit 动作落事件
    后，应改读 human.rewrite 事件而不是继续用这个代理指标。"""
    events_path = os.path.join(data_dir, "events.jsonl")
    audit_path = os.path.join(data_dir, "audit.jsonl")
    per_tool = {}
    turns = {}
    ai_writes = 0
    # events.jsonl 是首选事实源（有 traceId/turn/step）。旧实例还没落盘事件时
    # 退回 audit.jsonl —— 那里同样有 actorKind/cmd/ok/errorCode，不能因为
    # 新通道没开就报"零错误率"，那是最容易骗过自己的一种假绿。
    event_rows = 0
    if os.path.exists(events_path):
        for e in _reads(events_path):
            if e.get("type") != "command":
                continue
            event_rows += 1
            d = e.get("data") or {}
            cmd = d.get("command") or "?"
            _count_tool(per_tool, cmd, (not d.get("ok")) or (d.get("verdict") is False),
                        d.get("errorCode"))
            tid = e.get("traceId") or "-"
            turn = e.get("turn")
            if isinstance(turn, int) and turn > 0:
                turns.setdefault(tid, set()).add(turn)
            if _is_write(cmd):
                ai_writes += 1

    if not event_rows and os.path.exists(audit_path):
        for row in _reads(audit_path):
            if row.get("actorKind") != "ai":
                continue
            cmd = row.get("cmd") or "?"
            _count_tool(per_tool, cmd, not row.get("ok"), row.get("errorCode"))
            if _is_write(cmd):
                ai_writes += 1

    for slot in per_tool.values():
        slot["errorRate"] = round(slot["errors"] / slot["total"], 4) if slot["total"] else 0.0

    human_rewrites = 0
    if os.path.exists(audit_path):
        for row in _reads(audit_path):
            if row.get("actorKind") != "human":
                continue
            if row.get("cmd") in UNDO_COMMANDS and row.get("ok"):
                human_rewrites += 1

    counts = [len(v) for v in turns.values()]
    return {
        "perToolErrorRate": per_tool,
        "turnsPerTask": {
            "tasks": len(counts),
            "average": round(sum(counts) / len(counts), 2) if counts else 0.0,
            "max": max(counts) if counts else 0,
        },
        "humanRewriteRate": {
            "rewrites": human_rewrites,
            "aiWrites": ai_writes,
            "rate": round(human_rewrites / ai_writes, 4) if ai_writes else 0.0,
            "definition": "人工 file.undo / bmk.undo / dev.rollback 成功数 ÷ AI 写命令事件数",
            "proxy": True,
        },
    }
