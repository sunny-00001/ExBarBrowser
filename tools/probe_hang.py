"""启动卡死探针：只读进程/窗口状态，不发合成输入、不动光标、不抢焦点。

用法：python tools/probe_hang.py [pid]
输出：UI 线程是否响应、CPU 时间增量、窗口树与控件文字（找"正在处理"落在哪）。
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

WM_NULL = 0x0000
# 不用 SMTO_ABORTIFHUNG：窗口一旦被系统标记过 hung，那个标记会留着，
# 该标志会让调用**立刻**失败 —— 即使线程早就恢复了（实测把"已恢复"误报成
# "仍卡死"，日志里定时器明明还在跑）。判据要的是"此刻能不能处理消息"。
SMTO_NORMAL = 0x0000

# 必须显式声明签名：不声明时 ctypes 把所有参数按 int 传，
# byref(res) 这类指针参数会被截断/错传，SendMessageTimeout 恒返回 0，
# 于是"每个窗口都无响应"—— 判据变成空判据（负向验证踩过这个坑）。
u32.SendMessageTimeoutW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM, wt.UINT,
                                    wt.UINT, ctypes.POINTER(ctypes.c_size_t)]
u32.SendMessageTimeoutW.restype = ctypes.c_size_t
u32.GetWindowTextLengthW.argtypes = [wt.HWND]
u32.GetWindowTextLengthW.restype = ctypes.c_int
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.restype = ctypes.c_int
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetClassNameW.restype = ctypes.c_int
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.IsWindowVisible.restype = wt.BOOL
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowThreadProcessId.restype = wt.DWORD
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
u32.EnumWindows.restype = wt.BOOL
u32.EnumChildWindows.argtypes = [wt.HWND, WNDENUMPROC, wt.LPARAM]
u32.EnumChildWindows.restype = wt.BOOL
u32.IsHungAppWindow.argtypes = [wt.HWND]
u32.IsHungAppWindow.restype = wt.BOOL

# 进程/线程信息同理：不声明 restype 时句柄被当 int 截断成 32 位。
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.GetProcessTimes.argtypes = [wt.HANDLE, ctypes.POINTER(wt.FILETIME),
                                ctypes.POINTER(wt.FILETIME), ctypes.POINTER(wt.FILETIME),
                                ctypes.POINTER(wt.FILETIME)]
k32.GetProcessTimes.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CloseHandle.restype = wt.BOOL
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


def find_pid(name="ExBarBrowser.exe"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="replace").stdout
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[1].isdigit():
            pids.append(int(parts[1]))
    return pids


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


k32.Thread32First.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.Thread32First.restype = wt.BOOL
k32.Thread32Next.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.Thread32Next.restype = wt.BOOL


def proc_times(pid):
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        return None
    c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    ok = k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    k32.CloseHandle(h)
    if not ok:
        return None
    return ((k.dwHighDateTime << 32) | k.dwLowDateTime) / 1e7


def thread_count(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    if snap == -1:
        return []
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            ids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return ids


def window_text(hwnd):
    n = u32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    u32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def dump_windows(pid):
    rows = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            rows.append((hwnd, class_name(hwnd), window_text(hwnd),
                         bool(u32.IsWindowVisible(hwnd))))
        return True

    u32.EnumWindows(cb, 0)
    return rows


def dump_children(hwnd, depth=0, out=None):
    out = out if out is not None else []
    kids = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        kids.append(h)
        return True

    u32.EnumChildWindows(hwnd, cb, 0)
    for h in kids:
        out.append((depth, h, class_name(h), window_text(h), bool(u32.IsWindowVisible(h))))
        if depth < 3:
            dump_children(h, depth + 1, out)
    return out


def main():
    pids = [int(sys.argv[1])] if len(sys.argv) > 1 else find_pid()
    if not pids:
        print("没有找到 ExBarBrowser 进程")
        return 1
    pid = pids[0]
    print(f"pid={pid}")

    t0 = proc_times(pid)
    th0 = thread_count(pid)
    time.sleep(2.0)
    t1 = proc_times(pid)
    th1 = thread_count(pid)
    if t0 is None or t1 is None:
        print("读不到进程 CPU 时间（权限不足）")
    else:
        print(f"CPU 时间增量：{t1 - t0:.3f}s / 2s  →  "
              f"{'死循环（持续占 CPU）' if (t1 - t0) > 0.5 else '空闲（更像死锁/等锁）'}")
    print(f"线程数：{len(th0)} → {len(th1)}")

    wins = dump_windows(pid)
    print(f"\n顶层窗口 {len(wins)} 个：")
    for hwnd, cls, txt, vis in wins:
        print(f"  {hwnd:#010x} {cls:<32} vis={int(vis)} '{txt}'")

    # UI 线程响应性：对每个可见顶层窗口发 WM_NULL 并等 1000ms
    # （超时给足：800ms 的状态栏定时器本身就要几毫秒，300ms 太紧）
    print("\nUI 线程响应性：")
    for hwnd, cls, txt, vis in wins:
        if not vis:
            continue
        res = ctypes.c_size_t()
        r = u32.SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 1000,
                                    ctypes.byref(res))
        hung_flag = u32.IsHungAppWindow(hwnd)
        print(f"  {hwnd:#010x} {cls:<32} "
              f"{'无响应（真 hung）' if not r else '有响应'}"
              f"（系统 hung 标记={hung_flag}，历史遗留不等于现在卡）")

    main_wins = [w for w in wins if w[3] and "ExBarBrowser" in w[1]]
    for hwnd, cls, txt, vis in main_wins[:1]:
        print(f"\n主窗口 {hwnd:#010x} 控件树（找“正在处理”）：")
        for depth, h, c, t, v in dump_children(hwnd):
            if t.strip():
                print(f"  {'  ' * depth}{c:<28} vis={int(v)} '{t[:80]}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
