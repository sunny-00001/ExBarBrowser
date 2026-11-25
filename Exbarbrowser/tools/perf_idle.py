# -*- coding: utf-8 -*-
# 空闲态 CPU 基线：不发合成输入，只读；不动光标、不抢焦点。
# 启动独立实例打开大目录 → 等首帧 + 稳定 → 采样 5s 进程 CPU 时间 → WM_CLOSE 收尾。
# 用来验证 P0-2：状态栏 800ms 定时器在“什么都没变”时不再做全量取数。
# 用法：
#   python tools/perf_idle.py --exe ExBarBrowser.new.exe --label "P0前" --save data/idle-before.json
#   python tools/perf_idle.py --exe ExBarBrowser.p0.exe   --label "P0后" --save data/idle-after.json
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
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.PostMessageW.restype = ctypes.c_long
u.IsWindow.argtypes = [wt.HWND]
u.IsWindow.restype = wt.BOOL
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", wt.DWORD), ("dwHighDateTime", wt.DWORD)]


k.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k.OpenProcess.restype = wt.HANDLE
k.GetProcessTimes.argtypes = [wt.HANDLE, ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
                              ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME)]
k.CloseHandle.argtypes = [wt.HANDLE]


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


def log_ready(off, marker):
    try:
        with open(log_path(), "rb") as f:
            f.seek(off)
            return marker in f.read().decode("utf-16-le", errors="ignore")
    except Exception:
        return False


def filetime_100ns(ft):
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


def cpu_100ns(pid):
    h = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        creation, exit_, kernel, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not k.GetProcessTimes(h, ctypes.byref(creation), ctypes.byref(exit_),
                                 ctypes.byref(kernel), ctypes.byref(user)):
            return None
        return filetime_100ns(kernel) + filetime_100ns(user)
    finally:
        k.CloseHandle(h)


def one_run(exe, path, sample_seconds, settle_seconds):
    try:
        off = os.path.getsize(log_path())
    except Exception:
        off = 0

    env = os.environ.copy()
    env["EXBARBROWSER_MULTI"] = "1"
    proc = subprocess.Popen([exe, "--ai", path], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    hwnd = None
    deadline = time.time() + 20
    while time.time() < deadline:
        hwnd = find_window(proc.pid)
        if hwnd:
            break
        if proc.poll() is not None:
            return {"error": "窗口出现前退出", "code": proc.returncode}
        time.sleep(0.02)
    if not hwnd:
        proc.terminate()
        return {"error": "窗口未出现"}

    deadline = time.time() + 15
    while time.time() < deadline and not log_ready(off, "首次焦点已给出"):
        if proc.poll() is not None:
            return {"error": "首帧前退出", "code": proc.returncode}
        time.sleep(0.05)
    time.sleep(settle_seconds)

    c0 = cpu_100ns(proc.pid)
    t0 = time.perf_counter()
    time.sleep(sample_seconds)
    c1 = cpu_100ns(proc.pid)
    elapsed = time.perf_counter() - t0
    cpu_ms = None if c0 is None or c1 is None else max(0, c1 - c0) / 10000.0

    u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    deadline = time.perf_counter() + 20
    while time.perf_counter() < deadline and proc.poll() is None:
        time.sleep(0.002)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    return {
        "cpu_ms": None if cpu_ms is None else round(cpu_ms, 1),
        "elapsed_ms": round(elapsed * 1000, 1),
        "cpu_percent": None if cpu_ms is None else round(100.0 * cpu_ms / (elapsed * 1000), 2),
    }


def main():
    exe = os.path.abspath("ExBarBrowser.exe")
    path = r"C:\Windows\System32"
    runs = 3
    seconds = 5.0
    settle = 2.0
    label = ""
    save = None
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a == "--exe" and i + 1 < len(sys.argv):
            exe = os.path.abspath(sys.argv[i + 1]); i += 2
        elif a == "--path" and i + 1 < len(sys.argv):
            path = sys.argv[i + 1]; i += 2
        elif a == "--runs" and i + 1 < len(sys.argv):
            runs = int(sys.argv[i + 1]); i += 2
        elif a == "--seconds" and i + 1 < len(sys.argv):
            seconds = float(sys.argv[i + 1]); i += 2
        elif a == "--settle" and i + 1 < len(sys.argv):
            settle = float(sys.argv[i + 1]); i += 2
        elif a == "--label" and i + 1 < len(sys.argv):
            label = sys.argv[i + 1]; i += 2
        elif a == "--save" and i + 1 < len(sys.argv):
            save = sys.argv[i + 1]; i += 2
        else:
            i += 1

    print("label=%s exe=%s path=%s" % (label, exe, path))
    results = []
    for n in range(runs):
        r = one_run(exe, path, seconds, settle)
        results.append(r)
        print("  run %d: %s" % (n + 1, json.dumps(r, ensure_ascii=False)))
    ok = [r for r in results if r.get("cpu_ms") is not None]
    summary = {"label": label, "exe": exe, "path": path, "runs": results}
    if ok:
        vals = sorted(r["cpu_ms"] for r in ok)
        summary["median_cpu_ms"] = vals[len(vals) // 2]
        print("  ==> median CPU %.1f ms / %.1f s" % (summary["median_cpu_ms"], seconds))
    if save:
        os.makedirs(os.path.dirname(save) or ".", exist_ok=True)
        with open(save, "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
        print("  saved -> " + save)


if __name__ == "__main__":
    main()
