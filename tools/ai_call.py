#!/usr/bin/env python
# AI 通道最小客户端：读 profile 里的 pipe/token，走命名管道发请求。
# 用途：验证 view.pane.vertical 这类**运行时**命令真的生效（不靠模拟输入）。
#
# 两个坑（都实测踩过）：
#   1. 管道是**按连接实例化**的：服务端建实例 → 等连接 → 处理 → 关实例 → 再建下一个。
#      客户端正好落在间隙期时 CreateFile 会报 ERROR_FILE_NOT_FOUND（WinError 2），
#      所以必须轮询重试，不能一失败就报错。
#   2. 多实例共享同一 profile 时，规范文件 ai-pipe.txt 指向"最后启动的实例"。
#      要确认连的是哪个实例，从管道名尾部取 PID：
#      \\.\pipe\ExBarBrowser_ai_<hash>_<pid> → <pid>。
#
# 用法：
#   python tools/ai_call.py <profile 目录>            # 默认跑一轮 状态/能力/热切换
#   python tools/ai_call.py <profile 目录> --pid      # 只打印本实例 PID
import json
import os
import re
import sys
import time

profile = sys.argv[1] if len(sys.argv) > 1 else "."
pipe_file = os.path.join(profile, "ai-pipe.txt")
token_file = os.path.join(profile, "ai-token.txt")


def read_text(p):
    with open(p, "r", encoding="utf-8-sig") as f:
        return f.read().strip()


pipe_name = read_text(pipe_file)
token = read_text(token_file)
m = re.search(r"_(\d+)\s*$", pipe_name)
my_pid = int(m.group(1)) if m else 0
print("pipe  =", pipe_name)
print("pid   =", my_pid)
print("token =", token[:6] + "...")

if "--pid" in sys.argv:
    sys.exit(0)


def call(req):
    """发一条请求并读一行应答；管道实例不存在时轮询重试（最多 20 秒）。"""
    payload = json.dumps(req, ensure_ascii=False).encode("utf-8") + b"\n"
    deadline = time.time() + 20
    last = None
    while time.time() < deadline:
        try:
            f = open(pipe_name, "r+b", buffering=0)
        except OSError as e:
            last = e
            time.sleep(0.2)
            continue
        with f:
            f.write(payload)
            f.flush()
            buf = b""
            while time.time() < deadline:
                chunk = f.read(4096)
                if not chunk:
                    break
                buf += chunk
                if b"\n" in buf:
                    break
            if buf:
                return json.loads(buf.decode("utf-8").splitlines()[0])
        time.sleep(0.2)
    raise SystemExit("AI 通道连不上（管道 %s）：%s" % (pipe_name, last))


st = call({"method": "state", "token": token})
print("\n[state before] dualPane=%s paneVertical=%s activePane=%s paneSplit=%s"
      % (st.get("dualPane"), st.get("paneVertical"), st.get("activePane"), st.get("paneSplit")))

caps = call({"method": "capabilities", "token": token})
blob = json.dumps(caps, ensure_ascii=False)
print("[capabilities] 含 view.pane.vertical =", "view.pane.vertical" in blob)

r = call({"method": "tools/call", "name": "config.set",
          "args": {"key": "paneVertical", "value": False}, "token": token})
print("[cmd config.set paneVertical=false] ok=%s %s" % (r.get("ok"), r.get("error") or ""))

st2 = call({"method": "state", "token": token})
print("[state after ] dualPane=%s paneVertical=%s activePane=%s"
      % (st2.get("dualPane"), st2.get("paneVertical"), st2.get("activePane")))

r2 = call({"method": "tools/call", "name": "config.set",
           "args": {"key": "paneVertical", "value": True}, "token": token})
print("[cmd config.set paneVertical=true ] ok=%s" % r2.get("ok"))
st3 = call({"method": "state", "token": token})
print("[state back  ] dualPane=%s paneVertical=%s" % (st3.get("dualPane"), st3.get("paneVertical")))
