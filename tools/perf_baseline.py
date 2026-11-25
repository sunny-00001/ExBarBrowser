# -*- coding: utf-8 -*-
# 响应速度基线采集：不发合成输入，只读；不动光标、不抢焦点。
# 启动独立实例 → 等首帧 → 从日志 [+N ms] 打点解析启动各阶段 → WM_CLOSE 触发关闭 → 记录关闭耗时。
# 用法：
#   python tools/perf_baseline.py --runs 3 --label "优化前" --save data/perf-baseline-before.json
#   python tools/perf_baseline.py --runs 3 --label "优化后" --save data/perf-baseline-after.json
#   python tools/perf_baseline.py --exe ExBarBrowser.new.exe   # 主 exe 被占用时测新产物
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import socket
import subprocess
import sys
import time
import statistics

u = ctypes.windll.user32
WM_CLOSE = 0x0010
CLASS = "ExBarBrowser_Main"

u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.PostMessageW.restype = ctypes.c_long
u.IsWindow.argtypes = [wt.HWND]
u.IsWindow.restype = wt.BOOL
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

# 启动链路关键打点：日志子串 → 阶段名
STAGES = [
    ("[boot] Config 已加载", "config"),
    ("[boot] OnCreate 开始", "oncreate_begin"),
    ("[navpane] 原生导航树已创建", "navpane_tree"),
    ("[boot] navPane 建树完成", "navpane_done"),
    ("[boot] 四条栏已创建", "bars_done"),
    ("[boot] 书签已加载", "bookmarks_done"),
    ("[boot] OnCreate 结束", "oncreate_end"),
    ("[boot] 主窗口已创建", "mainwnd_created"),
    ("[init] Initialize hr=", "shell_init"),
    ("[nav] request path=", "nav_request"),
    ("[boot] 标签已建", "tabs_built"),
    ("[boot] 首帧布局完成", "layout_done"),
    ("[boot] 窗口已显示", "window_shown"),
    ("[boot] 首次焦点已给出", "first_focus"),
    ("[ai] 通道已启动", "ai_channel"),
    ("[aisvc] node 已启动", "node_started"),
    ("[kbhook] 已安装", "kbhook_ready"),
    ("[boot] 右视图已补建", "right_view"),
]

# 派生的"耗时块"：名称 → (起点阶段, 终点阶段)
BLOCKS = [
    ("oncreate_total", "oncreate_begin", "oncreate_end"),
    ("navpane_build", "oncreate_begin", "navpane_done"),
    ("bars_and_bookmarks", "navpane_done", "oncreate_end"),
    ("first_navigation", "mainwnd_created", "tabs_built"),
    ("layout_and_show", "tabs_built", "window_shown"),
    ("show_window_cost", "layout_done", "window_shown"),
    ("time_to_first_frame", "config", "window_shown"),
    ("ai_startup", "first_focus", "kbhook_ready"),
]


def find_window(pid):
    found = []

    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(hwnd, buf, 256)
        if buf.value == CLASS:
            p = wt.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid:
                found.append(hwnd)
                return False
        return True

    u.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else None


def log_path():
    return os.path.join(os.environ.get("TEMP", "/tmp"), "ExBarBrowser.log")


def read_new_log(off):
    try:
        with open(log_path(), "rb") as f:
            f.seek(off)
            return f.read().decode("utf-16-le", errors="ignore")
    except Exception:
        return ""


def parse_stages(text):
    # 定位本次会话：最后一个 "[boot] Config 已加载" 所在行的行首起
    idx = text.rfind("[boot] Config 已加载")
    if idx < 0:
        return {}
    line_start = text.rfind("\n", 0, idx) + 1
    seg = text[line_start:]
    ts = {}
    pat = re.compile(r"^\[\+\s*(\d+)\s*ms\]\s*(.*)$")
    for line in seg.splitlines():
        m = pat.match(line)
        if not m:
            continue
        ms, body = int(m.group(1)), m.group(2)
        # config 是首帧基准，强制记 0（它就是打点基准行）
        if "[boot] Config 已加载" in body:
            ts["config"] = 0
            continue
        for needle, name in STAGES:
            if name not in ts and needle in body:
                ts[name] = ms
                break
    return ts


def parse_node_port(text):
    m = re.search(r"\[aisvc\] node 已启动 pid=\d+ port=(\d+)", text)
    return int(m.group(1)) if m else None


def port_ready(port, timeout=8.0):
    """等 node 的 HTTP 端口真正可连，避免通知发到未就绪端口走满超时。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def derive_blocks(ts):
    out = {}
    for name, a, b in BLOCKS:
        if a in ts and b in ts:
            out[name] = ts[b] - ts[a]
    return out


def one_run(exe, need_ai=True):
    path = log_path()
    try:
        off = os.path.getsize(path)
    except Exception:
        off = 0

    env = os.environ.copy()
    env["EXBARBROWSER_MULTI"] = "1"
    args = [exe] + (["--ai"] if need_ai else [])
    proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    hwnd = None
    deadline = time.time() + 20
    while time.time() < deadline:
        hwnd = find_window(proc.pid)
        if hwnd:
            break
        if proc.poll() is not None:
            return {"error": "进程在窗口出现前退出", "code": proc.returncode}
        time.sleep(0.02)
    if not hwnd:
        proc.terminate()
        return {"error": "窗口未出现"}

    # 等首帧完成（日志出现"首次焦点已给出"），最多 15s
    deadline = time.time() + 15
    while time.time() < deadline:
        text = read_new_log(off)
        if "首次焦点已给出" in text and ("node 已启动" in text or not need_ai):
            break
        if proc.poll() is not None:
            break
        time.sleep(0.05)

    # 关键：等 node 的 HTTP 端口真正可连，再触发关闭。
    # 否则冷启动时 "node 已启动" 日志早于 listen 完成，WM_CLOSE 的通知
    # 发到未监听端口 → 走满 600ms+1000ms 超时，基线虚高。
    if need_ai:
        port = parse_node_port(read_new_log(off))
        if port:
            port_ready(port, timeout=10.0)
        time.sleep(0.15)   # 让服务稳定进入 accept

    text = read_new_log(off)
    ts = parse_stages(text)
    blocks = derive_blocks(ts)

    # 关闭：WM_CLOSE（非输入消息）
    t0 = time.perf_counter()
    u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    deadline = t0 + 20
    t_win = None
    while time.perf_counter() < deadline:
        if not u.IsWindow(hwnd):
            t_win = time.perf_counter()
            break
        time.sleep(0.001)
    deadline = t0 + 20
    t_exit = None
    while time.perf_counter() < deadline:
        if proc.poll() is not None:
            t_exit = time.perf_counter()
            break
        time.sleep(0.001)
    if t_exit is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    return {
        "stages_ms": ts,
        "blocks_ms": blocks,
        "window_close_ms": None if t_win is None else round((t_win - t0) * 1000, 1),
        "exit_ms": None if t_exit is None else round((t_exit - t0) * 1000, 1),
    }


def main():
    runs = 3
    label = ""
    save = None
    need_ai = True
    exe = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "ExBarBrowser.exe")
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a == "--runs" and i + 1 < len(sys.argv):
            runs = int(sys.argv[i + 1]); i += 2
        elif a == "--label" and i + 1 < len(sys.argv):
            label = sys.argv[i + 1]; i += 2
        elif a == "--save" and i + 1 < len(sys.argv):
            save = sys.argv[i + 1]; i += 2
        elif a == "--exe" and i + 1 < len(sys.argv):
            exe = os.path.abspath(sys.argv[i + 1]); i += 2
        elif a == "--no-ai":
            need_ai = False; i += 1
        else:
            i += 1

    results = []
    print("label=%s  ai=%s" % (label, need_ai))
    for n in range(runs):
        r = one_run(exe, need_ai)
        results.append(r)
        b = r.get("blocks_ms", {})
        print("  run %d: ttff=%s oncreate=%s nav=%s show=%s | close(win=%s exit=%s)" % (
            n + 1, b.get("time_to_first_frame"), b.get("oncreate_total"),
            b.get("first_navigation"), b.get("show_window_cost"),
            r.get("window_close_ms"), r.get("exit_ms")))

    ok = [r for r in results if "blocks_ms" in r and r.get("exit_ms") is not None]
    summary = {"label": label, "ai": need_ai, "runs": results}
    if ok:
        agg = {}
        for key in ["time_to_first_frame", "oncreate_total", "navpane_build",
                    "first_navigation", "layout_and_show", "show_window_cost",
                    "ai_startup"]:
            vals = [r["blocks_ms"][key] for r in ok if key in r["blocks_ms"]]
            if vals:
                agg[key] = {
                    "median": round(statistics.median(vals), 1),
                    "min": round(min(vals), 1),
                    "max": round(max(vals), 1),
                }
        agg["exit_ms"] = {
            "median": round(statistics.median([r["exit_ms"] for r in ok]), 1),
            "min": round(min(r["exit_ms"] for r in ok), 1),
        }
        summary["aggregate"] = agg
        print("\n  基线（中位数 ms）:")
        for k, v in agg.items():
            print("    %-24s median=%6.1f  min=%6.1f" % (k, v["median"], v["min"]))

    if save:
        os.makedirs(os.path.dirname(save) or ".", exist_ok=True)
        with open(save, "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
        print("\n  saved -> " + save)


if __name__ == "__main__":
    main()
