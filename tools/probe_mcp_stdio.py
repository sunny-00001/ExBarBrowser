#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MCP over stdio 端到端验证。

用真实进程 + 真实管道跑一遍 MCP 握手：
  initialize → notifications/initialized → tools/list → tools/call → exbar/state → 断开

不注入键鼠（AGENTS.md R21）：全程只走 stdio 与命名管道。

用法：
  python3 tools/probe_mcp_stdio.py [ExBarBrowser.exe 路径]
"""
import json
import os
import subprocess
import sys
import time

EXE = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\NoteBook\AppData\Local\Temp\ebb_mcp\ExBarBrowser.exe"

fails = []


def rpc(proc, obj, expect=True):
    """发一条 JSON-RPC，等一条响应。"""
    line = json.dumps(obj, ensure_ascii=False)
    proc.stdin.write(line.encode("utf-8") + b"\n")
    proc.stdin.flush()
    if not expect:
        return None
    raw = proc.stdout.readline()
    if not raw:
        raise SystemExit("FAIL: 服务端没有回响应（进程可能已退出）")
    return json.loads(raw.decode("utf-8"))


def check(cond, msg):
    if cond:
        print("  OK", msg)
    else:
        fails.append(msg)
        print("  FAIL", msg)


def main():
    p = subprocess.Popen([EXE, "--mcp-stdio"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         cwd=os.path.dirname(EXE))
    try:
        # ---- 1. initialize ----
        init = rpc(p, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                       "params": {"protocolVersion": "2024-11-05",
                                  "capabilities": {},
                                  "clientInfo": {"name": "probe", "version": "1"}}})
        print("initialize →", json.dumps(init, ensure_ascii=False)[:220])
        check(init.get("jsonrpc") == "2.0", "是 JSON-RPC 2.0 响应")
        check(init.get("id") == 1, "id 回显正确")
        result = init.get("result") or {}
        check(result.get("protocolVersion") == "2024-11-05", "协议版本 2024-11-05")
        check("tools" in (result.get("capabilities") or {}), "声明了 tools 能力")
        check(bool((result.get("serverInfo") or {}).get("name")), "有 serverInfo")

        # ---- 2. notifications/initialized（通知，不该有响应）----
        p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized",
                                  "params": {}}).encode("utf-8") + b"\n")
        p.stdin.flush()

        # ---- 3. tools/list ----
        lst = rpc(p, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        tools = (lst.get("result") or {}).get("tools") or []
        print(f"tools/list → {len(tools)} 个工具")
        check(len(tools) > 100, f"工具数 {len(tools)} > 100")
        names = {t.get("name") for t in tools}
        check("fs.open" in names, "含 fs.open")
        check("tool.list" in names, "含 tool.list（只读工具）")
        first = tools[0]
        check(all(k in first for k in ("name", "description", "inputSchema")),
              "工具形状符合 MCP（name/description/inputSchema）")

        # ---- 4. tools/call（只读的 tool.list）----
        call = rpc(p, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                       "params": {"name": "tool.list", "arguments": {}}})
        res = call.get("result") or {}
        content = res.get("content") or []
        print("tools/call(tool.list) → isError =", res.get("isError"),
              "content 长度", len(content[0].get("text", "")) if content else 0)
        check(bool(content) and content[0].get("type") == "text", "返回了 content 文本块")
        check(res.get("isError") is False, "只读工具调用成功")
        check("structuredContent" in res, "带 structuredContent（MCP 结构化输出）")

        # ---- 5. 未知工具 → 必须报失败，不能崩 ----
        bad = rpc(p, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                      "params": {"name": "no.such.tool", "arguments": {}}})
        print("tools/call(未知工具) →", json.dumps(bad, ensure_ascii=False)[:170])
        bres = bad.get("result") or bad.get("error") or {}
        check(bool(bres), "未知工具返回了错误信息")
        check(bres.get("isError") is True or "error" in bad, "未知工具被标记为失败")
        check(bres.get("errorCode") == "unknown_tool",
              "errorCode 归类为 unknown_tool（唯一失败分类来源）")

        # ---- 6. exbar/state（厂商扩展方法，不是工具）----
        st = rpc(p, {"jsonrpc": "2.0", "id": 5, "method": "exbar/state", "params": {}})
        print("exbar/state →", json.dumps(st, ensure_ascii=False)[:170])
        check(st.get("id") == 5, "exbar/state 有响应且 id 正确")

        # ---- 7. 通知不该有响应：再问一次，id 必须对得上 ----
        nxt = rpc(p, {"jsonrpc": "2.0", "id": 6, "method": "tools/list", "params": {}})
        check(nxt.get("id") == 6, f"响应没串位（收到 id={nxt.get('id')}）")
        print("  OK 通知没有污染响应流")

        print()
        if fails:
            print(f"失败 {len(fails)} 项：")
            for f in fails:
                print("  -", f)
            raise SystemExit(1)
        print("全部通过：MCP over stdio 可用")
    finally:
        try:
            p.stdin.close()
        except Exception:
            pass
        # 客户端断开 = stdin EOF = 进程应当自己干净退出（退出码 0）。
        try:
            p.wait(timeout=15)
            print(f"退出码 = {p.returncode}")
            if p.returncode != 0:
                print("  FAIL 进程没有干净退出")
                raise SystemExit(1)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            print("  FAIL 断开 stdin 后进程没有退出")
            raise SystemExit(1)


if __name__ == "__main__":
    main()
