#!/usr/bin/env python3
# agentctl —— 闭环自举与"能力缺口"诊断。
#
# 为什么需要这个文件：新开一次对话时，AI 面对的是"程序可能没开、可能跑的是旧 exe、
# 管道文件可能是上次残留、想要的能力可能压根没暴露"这四种混合状态。没有统一入口时，
# 每个会话都要重新即兴推导一遍（先 build？还是先启动？pid 对得上吗？），
# 推导错一次的代价是"调用了旧代码却以为在验证新代码"——最难查的那类假绿。
#
# 本文件只做三件事，全部基于既有的结构化通道（tools/ebbctl.py），不新开旁路：
#   serve/up/status/down   进程与通道的生命周期，**不碰用户的窗口**（只关自己启的那一个）
#   tools/call             与 ebbctl 同语义，额外带"第几次重试"与数据目录自动发现
#   gap                    按产品返回的 errorCode 分类，翻译成"缺哪类能力 + 补哪几处代码"
#   experience/stats       跨会话经验（先验证再入库 + 可疑隔离 + 上限淘汰）与 B7 统计
#                          统计优先读 events.jsonl，旧实例回退 audit.jsonl，不报假零
#
# R21：全程只走命名管道，不发任何输入消息、不动光标、不抢焦点。
# 关闭实例用 WM_CLOSE（非输入消息，允许），且只对 runtime/.agentctl.pid 记录的自有实例。
#
# 两种启动方式的区别（踩过）：
#   serve  前台常驻，父进程活着就替你把实例按住。**在会回收子进程的环境里（IDE/沙箱命令）
#          必须用后台任务跑它**，否则命令一结束实例就被连带收走。
#   up     一次性自举，就绪即退；只适合人手动在终端里用，或在能保活的环境里用。
#          up 会自检"实例是否真的活过了命令结束"，不活就明说 launch_not_persistent。
#
# profile 根：默认程序根；EXBARBROWSER_MULTI=1 切到 profiles\multi（见 identity.h）。
# 发现文件在 <profile 根>\runtime\，旧布局在 <profile 根>\data\。本文件在候选
# profile 根里找"pid 还活着"的那一份，不写死某一层。
#
# 用法：
#   python tools/agentctl.py serve [--no-build] [--wait 30]   # 建议：后台任务
#   python tools/agentctl.py status
#   python tools/agentctl.py down [--force]
#   python tools/agentctl.py tools [--grep bmk]
#   python tools/agentctl.py call bmk.list '{}' [--approve] [--retry 2]
#   python tools/agentctl.py gap bmk.move '{"id":"b-14"}'
#   python tools/agentctl.py experience add bmk.list --insight "先列书签再下命令"
#   python tools/agentctl.py experience ls [--all]
#   python tools/agentctl.py stats
#
# 退出码：0 达成；1 未达成（verdict=false / 有缺口）；2 环境问题（通道/构建）。
import argparse
import ctypes
import ctypes.wintypes as wt
import difflib
import io
import json
import os
import re
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)
from ebbctl import Client, ChannelError, runtime_dir_of  # noqa: E402
import experience  # noqa: E402

# DATA 保留变量名以兼容大量调用点，语义是 **profile 根**（默认程序根）。
DATA = ROOT
EXE = os.path.join(ROOT, "ExBarBrowser.exe")
PID_FILE = os.path.join(ROOT, "runtime", ".agentctl.pid")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_u32 = ctypes.WinDLL("user32", use_last_error=True)
_k32.OpenProcess.restype = ctypes.c_void_p
_k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_k32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.DWORD)]
_k32.CloseHandle.argtypes = [ctypes.c_void_p]
_u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

_PROCESS_QUERY_LIMITED = 0x1000
_STILL_ACTIVE = 259
_WM_CLOSE = 0x0010
_DETACHED = 0x00000008
_NEW_GROUP = 0x00000200

# 管道名形如 \\.\pipe\ExBarBrowser_ai_4103c_4596：hwnd 十六进制（会带 a-f），pid 十进制。
_PIPE_RE = re.compile(r"ExBarBrowser_ai_([0-9A-Fa-f]+)_(\d+)\s*$")


# ---------------------------------------------------------------- Win32 小工具

def pid_alive(pid):
    """进程是否还活着。用 ExitCode==STILL_ACTIVE 而不是"OpenProcess 是否成功"：
    句柄可能因权限/时序短暂拿不到，拿到句柄也不代表进程还在跑。"""
    if not pid:
        return False
    h = _k32.OpenProcess(_PROCESS_QUERY_LIMITED, False, int(pid))
    if not h:
        return False
    try:
        code = wt.DWORD(0)
        if not _k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code)):
            return False
        return code.value == _STILL_ACTIVE
    finally:
        _k32.CloseHandle(ctypes.c_void_p(h))


def post_close(hwnd):
    """请求窗口自己关。为什么不用 taskkill：强制终止不给程序存会话/刷盘的机会，
    而 WM_CLOSE 走的是用户点 × 的同一条路径（R21 明确允许非输入消息）。"""
    return bool(_u32.PostMessageW(wt.HWND(int(hwnd)), _WM_CLOSE, 0, 0))


# --------------------------------------------------- 发现文件（profile 感知）

def profile_dirs(data_dir=None):
    """候选 profile 根。多实例 profile 优先：MULTI 启动的实例把发现文件写在
    <root>\\profiles\\multi\\runtime\\；末尾两个是旧布局回退，升级期仍能连上旧实例。
    """
    if data_dir:
        return [data_dir]
    return [os.path.join(ROOT, "profiles", "multi"), ROOT,
            os.path.join(ROOT, "data", "profile-multi"), os.path.join(ROOT, "data")]


def pipe_of(data_dir):
    """解析 <profile 根>/runtime/ai-pipe.txt -> dict(...)，找不到返回 None。

    管道名里带 hwnd 与 pid 是既有契约（identity.h 的命名派生），这里借它判断
    "还是不是这个进程"，以及优雅关闭时该找哪个窗口。
    """
    runtime = runtime_dir_of(data_dir)
    f = os.path.join(runtime, "ai-pipe.txt")
    if not os.path.isfile(f):
        return None
    try:
        pipe = io.open(f, encoding="utf-8").read().strip()
    except OSError:
        return None
    m = _PIPE_RE.search(pipe)
    if not m:
        return None
    pid = int(m.group(2))
    return {"dir": data_dir, "runtime": runtime, "pipe": pipe,
            "hwnd": int(m.group(1), 16), "pid": pid, "alive": pid_alive(pid)}


def endpoints(data_dir=None):
    """所有候选目录的规范发现文件，活的排前面（按文件时间）。"""
    eps = [e for e in (pipe_of(d) for d in profile_dirs(data_dir)) if e]
    eps.sort(key=lambda e: (e["alive"], os.path.getmtime(os.path.join(e["runtime"], "ai-pipe.txt"))),
             reverse=True)
    return eps


def live_endpoint(data_dir=None, timeout_s=0.0):
    """连得上、pid 也活着的那个端点；返回 (endpoint, tools_count) 或 (None, 0)。

    "文件存在"不等于"通道可用"：实例退出时可能来不及删文件，残留文件会让客户端
    连一个不存在的管道（err=2）。所以必须真连一次。
    """
    deadline = time.time() + max(0.0, timeout_s)
    while True:
        for ep in endpoints(data_dir):
            if not ep["alive"]:
                continue
            try:
                r = Client(ep["dir"]).list_tools()
                if r.get("ok") is not False:
                    return ep, len(r.get("tools") or [])
            except ChannelError:
                continue
        if time.time() >= deadline:
            return None, 0
        time.sleep(0.25)


def newest_source_mtime():
    """源码与构建脚本里最新的修改时间，用来回答"exe 是不是旧的"。"""
    newest, newest_f = 0.0, ""
    for base, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                p = os.path.join(base, f)
                t = os.path.getmtime(p)
                if t > newest:
                    newest, newest_f = t, p
    p = os.path.join(ROOT, "build_fast.py")
    if os.path.isfile(p) and os.path.getmtime(p) > newest:
        newest, newest_f = os.path.getmtime(p), p
    return newest, newest_f


def snapshot(data_dir=None):
    src_t, src_f = newest_source_mtime()
    exe_t = os.path.getmtime(EXE) if os.path.isfile(EXE) else 0.0
    ep, n = live_endpoint(data_dir)
    own = None
    if os.path.isfile(PID_FILE):
        try:
            own = int(io.open(PID_FILE, encoding="utf-8").read().strip())
        except (OSError, ValueError):
            own = None
    return {
        "exe_exists": os.path.isfile(EXE),
        "exe_time": _fmt(exe_t),
        "src_time": _fmt(src_t),
        "src_newest": os.path.relpath(src_f, ROOT) if src_f else None,
        # 1 秒容差：构建后源码与 exe 的 mtime 常常同秒
        "stale": bool(exe_t and src_t > exe_t + 1.0),
        "channel_ready": bool(ep),
        "data_dir": ep["dir"] if ep else None,
        "pipe": ep["pipe"] if ep else None,
        "hwnd": ep["hwnd"] if ep else None,
        "pid": ep["pid"] if ep else None,
        "tools": n,
        "own_pid": own,
        "own_pid_alive": pid_alive(own) if own else False,
    }


def _fmt(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else None


def ensure_built(no_build):
    """源码比 exe 新就构建。返回 (ok, 错误对象)。"""
    s = snapshot()
    if s["channel_ready"] and not s["stale"]:
        return True, None
    if not s["stale"] and s["exe_exists"]:
        return True, None
    if no_build:
        return False, {"error": "exe_stale_or_missing", "hint": "去掉 --no-build，或先 bash build.sh"}
    print("构建中（源码比 exe 新或 exe 缺失）…", file=sys.stderr)
    # 调 build_fast.py 而不是 build.sh：不依赖 bash 在 PATH 上，
    # 且构建清单只有 build_fast.py 一份（build.sh/bat 只是透传包装）。
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "build_fast.py")], cwd=ROOT)
    if rc != 0:
        return False, {"error": "build_failed", "rc": rc}
    # 有实例占着 exe 时构建脚本把新版本留成 .new.exe，目标仍是旧文件。
    # 不查这一条就会"以为在跑新代码"——本工具最该防的假绿。
    if snapshot()["stale"]:
        return False, {"error": "exe_replace_blocked",
                       "hint": "有实例占用 exe，新版本可能在 ExBarBrowser.new.exe；"
                               "关掉占用实例后重跑"}
    return True, None


def close_own():
    """关掉上次由 agentctl 启的实例（只认 pid 文件，绝不动用户手开的窗口）。"""
    if not os.path.isfile(PID_FILE):
        return None
    try:
        pid = int(io.open(PID_FILE, encoding="utf-8").read().strip())
    except (OSError, ValueError):
        return None
    if not pid_alive(pid):
        try:
            os.remove(PID_FILE)
        except OSError:
            pass
        return None
    for ep in endpoints():
        if ep["pid"] == pid:
            post_close(ep["hwnd"])
            break
    for _ in range(50):
        if not pid_alive(pid):
            try:
                os.remove(PID_FILE)
            except OSError:
                pass
            return pid
        time.sleep(0.1)
    return pid


# ------------------------------------------------------------------- 子命令

def cmd_status(a):
    s = snapshot(a.data_dir)
    print(json.dumps(s, ensure_ascii=False, indent=2))
    if not s["exe_exists"]:
        print("结论：exe 不存在 —— python tools/agentctl.py serve", file=sys.stderr)
        return 2
    if not s["channel_ready"]:
        print("结论：通道不可用 —— 后台跑 python tools/agentctl.py serve", file=sys.stderr)
        return 2
    if s["stale"]:
        print("结论：通道可用，但 exe 比源码旧（跑的是旧逻辑）", file=sys.stderr)
        return 1
    print("结论：就绪（%d 个工具，profile=%s）" % (s["tools"], s["data_dir"]), file=sys.stderr)
    return 0


def _launch():
    """以 --ai + MULTI 启动，返回 Popen。

    MULTI 是刻意的：默认启动会把路径转发给用户已开的窗口，那样测的是用户的实例，
    不是本次构建的产物。MULTI 同时把 profile 切到 data\\profile-multi，
    不会动用户的 settings.json。
    """
    env = dict(os.environ, EXBARBROWSER_MULTI="1")
    return subprocess.Popen([EXE, "--ai"], cwd=ROOT, env=env,
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            creationflags=_DETACHED | _NEW_GROUP,
                            close_fds=True)


def cmd_up(a):
    ok, err = ensure_built(a.no_build)
    if not ok:
        print(json.dumps({"ok": False, **err}, ensure_ascii=False, indent=2))
        return 2
    ep, n = live_endpoint(a.data_dir)
    if ep and not snapshot(a.data_dir)["stale"]:
        print(json.dumps({"ok": True, "reused": True, **snapshot(a.data_dir)},
                         ensure_ascii=False, indent=2))
        return 0

    close_own()
    proc = _launch()
    os.makedirs(os.path.dirname(PID_FILE), exist_ok=True)
    io.open(PID_FILE, "w", encoding="utf-8").write(str(proc.pid))

    ep, n = live_endpoint(a.data_dir, timeout_s=a.wait)
    out = {"ok": bool(ep), "reused": False, "launched_pid": proc.pid, "waited_s": a.wait}
    out.update(snapshot(a.data_dir))
    print(json.dumps(out, ensure_ascii=False, indent=2))
    if not ep:
        print("通道 %d 秒内没起来：看 %%TEMP%%\\ExBarBrowser.log 的 [init]/[ai]" % a.wait,
              file=sys.stderr)
        return 2

    # 就绪不代表能活过这条命令：会被回收子进程的环境里，命令一结束实例就没了。
    # 与其让下一次调用莫名其妙 channel_down，不如当场说清楚该换 serve。
    time.sleep(2.0)
    if not pid_alive(proc.pid):
        print(json.dumps({"ok": False, "error": "launch_not_persistent",
                          "hint": "本环境会回收子进程：改用后台任务跑 "
                                  "python tools/agentctl.py serve"}, ensure_ascii=False))
        return 2
    return 0


def cmd_serve(a):
    ok, err = ensure_built(a.no_build)
    if not ok:
        print(json.dumps({"ok": False, **err}, ensure_ascii=False, indent=2), flush=True)
        return 2
    close_own()
    proc = _launch()
    os.makedirs(os.path.dirname(PID_FILE), exist_ok=True)
    io.open(PID_FILE, "w", encoding="utf-8").write(str(proc.pid))

    ep, n = live_endpoint(a.data_dir, timeout_s=a.wait)
    if not ep:
        print(json.dumps({"ok": False, "error": "channel_timeout",
                          "pid": proc.pid, "waited_s": a.wait,
                          "hint": "看 %TEMP%\\ExBarBrowser.log 的 [init]/[ai]"},
                         ensure_ascii=False), flush=True)
        return 2
    print(json.dumps({"ok": True, "serving": True, "pid": proc.pid, **snapshot(a.data_dir)},
                     ensure_ascii=False), flush=True)

    t0 = time.time()
    try:
        while True:
            if not pid_alive(proc.pid):
                print(json.dumps({"event": "exited", "pid": proc.pid,
                                  "uptime_s": round(time.time() - t0)},
                                 ensure_ascii=False), flush=True)
                return 1
            if a.max and time.time() - t0 > a.max:
                post_close(ep["hwnd"])
                print(json.dumps({"event": "max_lifetime", "pid": proc.pid},
                                 ensure_ascii=False), flush=True)
                return 0
            time.sleep(2.0)
    except KeyboardInterrupt:
        pid = close_own()
        print(json.dumps({"event": "closed", "pid": pid}, ensure_ascii=False), flush=True)
        return 0


def cmd_down(a):
    own = None
    if os.path.isfile(PID_FILE):
        try:
            own = int(io.open(PID_FILE, encoding="utf-8").read().strip())
        except (OSError, ValueError):
            own = None
    if not own or not pid_alive(own):
        print(json.dumps({"ok": True, "closed": None,
                          "note": "没有 agentctl 启的实例（用户自己的窗口不会被关）"},
                         ensure_ascii=False, indent=2))
        return 0
    ep = next((e for e in endpoints(a.data_dir) if e["pid"] == own), None)
    if ep:
        post_close(ep["hwnd"])
    elif a.force:
        subprocess.call(["taskkill", "/PID", str(own), "/F"])
    else:
        print(json.dumps({"ok": False, "error": "pipe_mismatch",
                          "hint": "找不到该 pid 的管道名；确需强杀加 --force"},
                         ensure_ascii=False, indent=2))
        return 2
    for _ in range(50):
        if not pid_alive(own):
            try:
                os.remove(PID_FILE)
            except OSError:
                pass
            print(json.dumps({"ok": True, "closed": own}, ensure_ascii=False))
            return 0
        time.sleep(0.1)
    print(json.dumps({"ok": False, "error": "close_timeout", "pid": own,
                      "hint": "可能有模态对话框拦住 WM_CLOSE"}, ensure_ascii=False))
    return 1


def _endpoint_or_none(data_dir):
    ep, _n = live_endpoint(data_dir)
    return ep


def _tools(data_dir=None):
    """tools/list -> {name: spec}。失败抛 ChannelError。"""
    ep, _n = live_endpoint(data_dir)
    if not ep:
        raise ChannelError("channel_down")
    r = Client(ep["dir"]).list_tools()
    if r.get("ok") is False:
        raise ChannelError(str(r.get("error")))
    return {t["name"]: t for t in (r.get("tools") or [])}


def cmd_tools(a):
    try:
        tools = _tools(a.data_dir)
    except ChannelError:
        print(json.dumps({"ok": False, "error": "channel_down",
                          "fix": "后台任务跑 python tools/agentctl.py serve"},
                         ensure_ascii=False, indent=2))
        return 2
    rows = []
    for name, t in sorted(tools.items()):
        if a.grep and a.grep.lower() not in name.lower():
            continue
        schema = t.get("inputSchema") or {}
        rows.append({"name": name, "risk": t.get("risk"),
                     "needsSelection": t.get("needsSelection", False),
                     "required": schema.get("required") or [],
                     "title": t.get("title")})
    print(json.dumps({"ok": True, "count": len(rows), "tools": rows},
                     ensure_ascii=False, indent=2))
    return 0


def _call(name, args, approve=False, data_dir=None):
    ep, _n = live_endpoint(data_dir)
    if not ep:
        return {"ok": False, "error": "channel_down"}
    try:
        return Client(ep["dir"]).call(name, args, approve=approve)
    except ChannelError as ex:
        return {"ok": False, "error": "channel_down", "detail": str(ex)}


def _state(data_dir=None):
    """读状态快照（旧格式 method=state，不是 tools/call）。"""
    ep, _n = live_endpoint(data_dir)
    if not ep:
        return {}
    try:
        return Client(ep["dir"]).state()
    except ChannelError:
        return {}


def cmd_call(a):
    try:
        args = json.loads(a.args or "{}")
    except json.JSONDecodeError as ex:
        print(json.dumps({"ok": False, "error": "bad_json_args", "detail": str(ex)},
                         ensure_ascii=False))
        return 2

    last, attempt = {}, 0
    for attempt in range(1, a.retry + 1):
        last = _call(a.name, args, a.approve, a.data_dir)
        # 只重试"环境没准备好"这一类；参数错/未暴露/效果不符，重试只是烧时间。
        if last.get("error") not in ("channel_down", "timeout"):
            break
        if attempt < a.retry:
            time.sleep(0.5 * attempt)

    summary = {"command": a.name, "attempts": attempt, "ok": last.get("ok"),
               "verdict": last.get("verdict", last.get("ok")),
               "error": last.get("error"), "assertions": last.get("assertions")}
    # errorCode 必须带上：它是**机器可读**的失败类别（bad_args / blocked /
    # danger_disabled / not_implemented…），而 error 是人读的中文。
    # 丢掉它，调用方就只能去匹配中文字符串 —— 文档一改就断（AGENTS.md §8.3）。
    # 本文件自己的 `gap` 子命令正是按 errorCode 分类的，没有它那条路走不通。
    if last.get("errorCode"):
        summary["errorCode"] = last.get("errorCode")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if last.get("error") == "channel_down":
        return 2
    return 0 if summary["verdict"] else 1


# --------------------------------------------------------------- 缺口诊断
#
# 分类的**唯一真源是产品返回的 errorCode**（src/ai_api.cpp），不是中文串、更不是
# 本文件的猜测。这里只做"码 -> 该改哪几处"的映射：它属于仓库知识（哪个文件、哪条规则），
# 不该塞进产品里。
_CODE_TO_REASON = {
    "unknown_tool": "unknown_tool",
    "not_exposed": "not_exposed",
    "not_implemented": "not_implemented",
    "bad_args": "bad_args",
    "schema_invalid": "schema_invalid",
    "danger_disabled": "approval_required",
    "need_approval": "approval_required",
    # "被拦住"不再是一锅端：四类的下一步完全不同（换目标 / 稍后重试 /
    # 问人 / 换一条路），映射到不同 reason 才能给出不同的处置建议。
    "precondition_unmet": "precondition_unmet",
    "lock_conflict": "lock_conflict",
    "canceled_by_user": "canceled_by_user",
    "not_supported": "not_supported",
    "timed_out": "timed_out",
    "failed": "failed",
    "effect_not_met": "effect_not_met",
    # 旧版本程序还会返回一锅端的 "blocked"。映射到最接近的一类，
    # 别让它掉进 "failed" —— 那会让 agent 以为是代码有 bug。
    "blocked": "precondition_unmet",
}


def _fix_for(reason, name):
    """缺口类型 -> 改哪几处。步骤号对应 AGENTS.md §5，不是复制粘贴用。"""
    table = {
        "channel_down": [
            "后台任务跑：python tools/agentctl.py serve",
            "仍不通就看 %TEMP%\\ExBarBrowser.log 的 [init] Initialize hr= 与 [ai] 行",
        ],
        "unknown_tool": [
            "命令表里没有这条：AGENTS.md §5「新增命令」1-5 步",
            "src/commands.h 枚举只在末尾追加（CmdId 稳定，不得重排）",
            "src/commands.cpp 的 kTable 加一行 → app_commands*.cpp 注册 handler",
            "带参数则在 src/ai_api.cpp 的 kArgKeys 加键（必须与 argsSchema.required 同名）",
            "python tools/check_commands.py && python tools/check_ai_meta.py",
        ],
        "not_exposed": [
            "命令已实现但没开放：src/commands.cpp 补 aiExposed=true",
            "补 argsSchema（合法 JSON）与 risk；description 写清「什么时候不要用」",
            "needsSelection=true 要求观测面能读选中（check_ai_meta 第 4/14 项会拦）",
            "python tools/check_ai_meta.py",
        ],
        "not_implemented": [
            "命令在表里但 implemented=false：写 handler，在 app_commands*.cpp 注册",
            "改完后把该条 implemented 置 true，再按 not_exposed 的流程决定是否开放",
            "python tools/check_commands.py（三方对账会拦住「表里有、处理器没有」）",
        ],
        "needs_selection": [
            "先只读确认目标（state/OpListDir），不要替用户猜选中项",
            "长期需要按参数指定目标就加确定式命令（Op*/Fs* 模式），不要把 UI 命令硬暴露",
        ],
        "bad_args": [
            "读 inputSchema：python tools/agentctl.py tools --grep " + name,
            "必填缺失/多传参数都是显式拒绝（不静默忽略），改请求而不是改代码",
        ],
        "schema_invalid": [
            "命令表里的 argsSchema 不是合法 JSON 对象 —— 这是实现缺陷",
            "改 src/commands.cpp 对应行，再 python tools/check_ai_meta.py（第 2 项会拦）",
        ],
        "approval_required": [
            "Write/Danger 需要人类批准：把命令与参数交给用户，等明确同意后再带 --approve",
            "danger_disabled 说明设置里 aiAllowDanger=false —— 这是用户的决定，不要自己改",
            "AI 不得自行加 approve —— 自我迭代不包含自我批准破坏性操作",
        ],
        # 【为什么分开】"被拦住"有四种，下一步完全不同。
        # 混成一条建议，agent 就会把"稍后重试"用在"能力不存在"上，
        # 或者把"该问人"当成"换个目标再来" —— 后者最糟，会一直骚扰用户。
        "precondition_unmet": [
            "前置条件不满足（error 里是 NoteFailure 的具体原因，不是 bug）",
            "先读状态确认前提，再决定是换目标还是先做前置动作",
            "改完再调一次；前提没变就再调只会一直失败",
        ],
        "lock_conflict": [
            "目标被占用或被锁（剪贴板、文件、目录）—— 执行期争用",
            "关掉占用的程序后重试；或换一个目标",
            "重试有意义：占用是暂时的（但别无脑连发，先看看占用还在不在）",
        ],
        "canceled_by_user": [
            "用户主动取消了这次操作（拒绝批准 / 在进度窗口上点了取消）",
            "**不要重试** —— 停下来问用户要不要继续",
            "换参数重来一次等于绕过用户的决定",
        ],
        "not_supported": [
            "这个环境/这个对象不支持这件事（视图模式不对、能力不存在）",
            "换一条路（换视图模式、换目标类型）；重试永远不会有用",
        ],
        "timed_out": [
            "等待时限到了，后台作业**仍在跑**（不是失败）",
            "用 tool.job.get 继续查那个 jobId，不要重发同一条命令",
            "要停它就用 tool.job.cancel",
        ],
        "blocked_ui_thread": [
            "10 秒超时=UI 线程卡在模态对话框（打开/另存/属性…）",
            "请人关掉对话框再重试；不要循环重发（只会连续超时）",
        ],
        "effect_not_met": [
            "ok=true 但 verdict=false：读 assertions 看哪条判据不满足",
            "先独立回读确认是否真的没生效（bmk.list / state）",
            "真没生效 → 修 handler；已生效 → 修验收判据（判据错也是 bug）",
        ],
        "failed": [
            "看 error 原文与 %TEMP%\\ExBarBrowser.log 对应模块行（[nav]/[tab]/[ops]）",
            "确认前置条件：路径存在、权限、目标未被占用",
        ],
        "ok": [],
    }
    return table.get(reason, ["未归类：把 errorCode 与复现步骤记进 CHANGELOG，"
                              "再对照 AGENTS.md §6 坑位表"])


def _reason_from(r):
    """从调用结果取缺口类别。优先 errorCode；老版本程序没有该字段时才回落到文案。"""
    code = r.get("errorCode")
    if code:
        return _CODE_TO_REASON.get(code, "failed")
    err = r.get("error") or ""
    if err == "timeout":
        return "blocked_ui_thread"
    if err.startswith("缺少必填参数") or err == "该命令不接受参数":
        return "bad_args"
    if err == "未知或未暴露给 AI 的命令":
        # 老版本不区分三态，只能退回"可能是没暴露"这个最保守的判断。
        return "not_exposed"
    return "failed"


def cmd_gap(a):
    try:
        args = json.loads(a.args or "{}")
    except json.JSONDecodeError as ex:
        print(json.dumps({"ok": False, "error": "bad_json_args", "detail": str(ex)},
                         ensure_ascii=False, indent=2))
        return 2

    try:
        tools = _tools(a.data_dir)
    except ChannelError:
        print(json.dumps({"reason": "channel_down", "command": a.name,
                          "fix": _fix_for("channel_down", a.name)},
                         ensure_ascii=False, indent=2))
        return 2

    name = a.name
    if name not in tools:
        # 不在 tools/list 里，但产品能说清是哪一类：直接问 capabilities，
        # 不再靠 grep 源码猜（那正是"外部工具替产品做判断"的坏味道）。
        cap = {}
        ep, _n = live_endpoint(a.data_dir)
        if ep:
            try:
                cap = Client(ep["dir"]).request({"method": "capabilities"})
            except ChannelError:
                cap = {}
        if name in (cap.get("notExposed") or []):
            reason = "not_exposed"
        elif name in (cap.get("notImplemented") or []):
            reason = "not_implemented"
        else:
            reason = "unknown_tool"
        out = {"reason": reason, "command": name,
               "evidence": {"in_tools_list": False,
                            "similar_tools": difflib.get_close_matches(
                                name, list(tools), n=3, cutoff=0.5)},
               "fix": _fix_for(reason, name)}
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 1

    spec = tools[name]

    # 依赖选中项但当前没有选中：这是"环境不满足"，不是代码缺失。
    # 先判再调 —— 有选中时再调会真的写东西，诊断工具不该有副作用。
    if spec.get("needsSelection"):
        st = _state(a.data_dir)
        payload = st.get("structuredContent") or st
        if not payload.get("selectionCount"):
            out = {"reason": "needs_selection", "command": name, "spec": spec,
                   "evidence": {"selectionCount": 0},
                   "fix": _fix_for("needs_selection", name)}
            print(json.dumps(out, ensure_ascii=False, indent=2))
            return 1

    r = _call(name, args, a.approve, a.data_dir)
    if r.get("error") == "channel_down":
        reason = "channel_down"
    elif r.get("ok") and r.get("verdict", True) is False:
        reason = "effect_not_met"
    elif r.get("ok"):
        reason = "ok"
    else:
        reason = _reason_from(r)

    out = {"reason": reason, "command": name, "spec": spec,
           "result": {k: r.get(k) for k in ("ok", "verdict", "error", "errorCode",
                                            "assertions")
                      if r.get(k) is not None},
           "fix": _fix_for(reason, name)}
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if reason == "ok" else 1


# ------------------------------------------------------- C5/C5b 经验沉淀

def _experience_verify(a, spec, proof):
    """先验证再入库：只读命令现场调一次；写命令必须拿 events.jsonl 里的
    真实成功事件当证据。绝不因为调用方自报"我成功了"就放进池子。"""
    if a.command not in spec:
        return False, "not_in_tools"
    if spec[a.command].get("annotations", {}).get("readOnlyHint"):
        r = _call(a.command, {}, False, a.data_dir)
        return bool(r.get("ok")), {"ok": bool(r.get("ok")),
                                   "errorCode": r.get("errorCode")}
    return experience.verify_against_events(a.data_dir, proof, a.command)


def cmd_experience(a):
    data_dir = a.data_dir or DATA
    act = a.experience_cmd
    if act == "ls":
        out = experience.list_entries(data_dir, a.all)
    elif act == "add":
        try:
            args = json.loads(a.args or "{}")
            proof = json.loads(a.proof or "{}") if a.proof else None
        except json.JSONDecodeError as ex:
            print(json.dumps({"ok": False, "reason": "bad_json", "detail": str(ex)},
                             ensure_ascii=False, indent=2))
            return 2
        try:
            spec = _tools(a.data_dir)
        except ChannelError:
            print(json.dumps({"ok": False, "reason": "channel_down"},
                             ensure_ascii=False, indent=2))
            return 2
        entry = {"kind": a.kind, "command": a.command, "insight": a.insight,
                 "args": args, "source": "agentctl"}
        if proof is not None:
            entry["proof"] = proof
        out = experience.add(data_dir, entry,
                             verify_fn=lambda c, _u: _experience_verify(a, spec, proof))
    elif act == "vote":
        out = experience.vote(data_dir, a.entry_id, a.direction)
    elif act == "edit":
        try:
            spec = _tools(a.data_dir)
        except ChannelError:
            print(json.dumps({"ok": False, "reason": "channel_down"},
                             ensure_ascii=False, indent=2))
            return 2
        out = experience.edit(data_dir, a.entry_id, a.insight,
                              verify_fn=lambda c, u: _experience_verify(a, spec, None))
    else:
        out = {"ok": False, "reason": "unknown_action"}
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if out.get("ok", True) else 1


def cmd_stats(a):
    print(json.dumps(experience.stats(a.data_dir or DATA), ensure_ascii=False, indent=2))
    return 0


# --------------------------------------------------------------------- 入口

def main(argv=None):
    ap = argparse.ArgumentParser(description="ExBarBrowser 闭环自举与缺口诊断")
    ap.add_argument("--profile", "--data-dir", dest="data_dir",
                    help="强制指定 profile 根（旧 --data-dir 兼容；默认自动发现）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="汇总 exe/源码新鲜度、进程与通道状态")

    p = sub.add_parser("serve", help="构建（仅过期时）+ 启动 + 常驻持有实例（建议后台任务）")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--wait", type=int, default=30, help="等通道就绪的秒数")
    p.add_argument("--max", type=int, default=0, help="最长持有秒数（0=不限）")

    p = sub.add_parser("up", help="一次性自举，就绪即退（会被回收子进程的环境请用 serve）")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--wait", type=int, default=30)

    p = sub.add_parser("down", help="关闭 agentctl 自己启的实例（不碰用户窗口）")
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("tools", help="列出 AI 可见命令")
    p.add_argument("--grep")

    p = sub.add_parser("call", help="调用一条命令")
    p.add_argument("name")
    p.add_argument("args", nargs="?", default="{}")
    p.add_argument("--approve", action="store_true")
    p.add_argument("--retry", type=int, default=1, help="仅对 channel_down/timeout 重试")

    p = sub.add_parser("gap", help="诊断一次失败属于哪类缺口，并给出补法")
    p.add_argument("name")
    p.add_argument("args", nargs="?", default="{}")
    p.add_argument("--approve", action="store_true")

    p = sub.add_parser("experience", help="跨会话经验沉淀（先验证再入库，含污染隔离）")
    ex = p.add_subparsers(dest="experience_cmd", required=True)
    p_add = ex.add_parser("add", help="新增一条经验；只读命令现场验证，写命令要 proof")
    p_add.add_argument("command")
    p_add.add_argument("--kind", choices=experience.KINDS, default="success")
    p_add.add_argument("--insight", required=True)
    p_add.add_argument("--args", default="{}", help="只读命令的验证参数（JSON 对象）")
    p_add.add_argument("--proof", help='写命令的成功证据：\'{"seq":N,"traceId":"..."}\'')
    p_ls = ex.add_parser("ls", help="列出活跃经验池与隔离/流水计数")
    p_ls.add_argument("--all", action="store_true", help="连隔离与 ledger 一起输出")
    p_vote = ex.add_parser("vote", help="UPVOTE / DOWNVOTE（ExpeL 口径）")
    p_vote.add_argument("entry_id")
    p_vote.add_argument("direction", choices=("up", "down"))
    p_edit = ex.add_parser("edit", help="改写经验；会重新验证，不能绕过入库门槛")
    p_edit.add_argument("entry_id")
    p_edit.add_argument("--insight", required=True)

    sub.add_parser("stats", help="每工具错误率 / 每任务轮数 / 人工改写率（B7）")

    a = ap.parse_args(argv)
    if a.cmd == "experience":
        return cmd_experience(a)
    if a.cmd == "stats":
        return cmd_stats(a)
    fn = {"status": cmd_status, "serve": cmd_serve, "up": cmd_up, "down": cmd_down,
          "tools": cmd_tools, "call": cmd_call, "gap": cmd_gap}[a.cmd]
    return fn(a)


if __name__ == "__main__":
    sys.exit(main())
