# -*- coding: utf-8 -*-
# 关闭耗时测量：不发合成输入，只读；不动光标、不抢焦点。
# 用 WM_CLOSE（允许的非输入消息）触发窗口关闭，测"点关闭 → 窗口销毁 → 进程退出"耗时。
# 用法：python tools/measure_close.py [--runs 3] [--label NAME] [--save FILE]
#       python tools/measure_close.py --no-ai   # 不开 AI 通道（node 不启动）作对照
#       python tools/measure_close.py --exe ExBarBrowser.new.exe  # 主 exe 被占用时测新产物
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

u = ctypes.windll.user32
k = ctypes.windll.kernel32

WM_CLOSE = 0x0010
CLASS = "ExBarBrowser_Main"

u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.PostMessageW.restype = ctypes.c_long   # wintypes 无 LRESULT，win64 下指针等宽
u.IsWindow.argtypes = [wt.HWND]
u.IsWindow.restype = wt.BOOL
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


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


def log_ready(path, off, marker):
    try:
        with open(path, "rb") as f:
            f.seek(off)
            text = f.read().decode("utf-16-le", errors="ignore")
        return marker in text
    except Exception:
        return False


def one_run(exe, use_ai, warmup):
    path = log_path()
    try:
        off = os.path.getsize(path)
    except Exception:
        off = 0

    env = os.environ.copy()
    env["EXBARBROWSER_MULTI"] = "1"          # 强制独立实例，不转发给已有窗口
    args = [exe]
    if use_ai:
        args.append("--ai")                  # 强制开 AI 通道 → 拉起 node 服务
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

    # 等 node 服务就绪（基线/优化都要 node 在跑，才有 aisvc::Stop 的等待）
    if use_ai:
        deadline = time.time() + 15
        while time.time() < deadline and not log_ready(path, off, "node 已启动"):
            time.sleep(0.05)
        time.sleep(warmup)                    # 让服务完全稳定

    t0 = time.perf_counter()
    u.PostMessageW(hwnd, WM_CLOSE, 0, 0)

    # 窗口销毁
    deadline = t0 + 20
    t_win = None
    while time.perf_counter() < deadline:
        if not u.IsWindow(hwnd):
            t_win = time.perf_counter()
            break
        time.sleep(0.001)

    # 进程退出
    deadline = t0 + 20
    t_exit = None
    while time.perf_counter() < deadline:
        if proc.poll() is not None:
            t_exit = time.perf_counter()
            break
        time.sleep(0.001)

    if t_exit is None:
        proc.terminate()                      # 兜底，避免卡死残留
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        return {"error": "进程未在 20s 内退出"}

    return {
        "window_ms": None if t_win is None else round((t_win - t0) * 1000, 1),
        "exit_ms": round((t_exit - t0) * 1000, 1),
    }


def main():
    runs = 3
    label = ""
    save = None
    use_ai = True
    warmup = 0.4
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
            use_ai = False; i += 1
        elif a == "--warmup" and i + 1 < len(sys.argv):
            warmup = float(sys.argv[i + 1]); i += 2
        else:
            i += 1

    results = []
    print("label=%s  ai=%s  exe=%s" % (label, use_ai, exe))
    for n in range(runs):
        r = one_run(exe, use_ai, warmup)
        results.append(r)
        print("  run %d: %s" % (n + 1, json.dumps(r, ensure_ascii=False)))

    ok = [r for r in results if "exit_ms" in r and "error" not in r]
    summary = {"label": label, "ai": use_ai, "runs": results}
    if ok:
        summary["avg_exit_ms"] = round(sum(r["exit_ms"] for r in ok) / len(ok), 1)
        wins = [r["window_ms"] for r in ok if r.get("window_ms") is not None]
        if wins:
            summary["avg_window_ms"] = round(sum(wins) / len(wins), 1)
        print("  ==> avg window=%s ms  avg exit=%s ms" %
              (summary.get("avg_window_ms"), summary["avg_exit_ms"]))
    else:
        print("  ==> 无有效样本")

    if save:
        with open(save, "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
        print("  saved -> " + save)


if __name__ == "__main__":
    main()
