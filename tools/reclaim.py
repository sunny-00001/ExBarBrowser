#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""构建残留与会话临时目录的回收器（唯一实现）。

## 为什么需要它

`build_fast.py` 的 `--objdir` / `--out` 让多会话可以并行构建，代价是**每开一个
隔离构建就留下一个 101 MB 的 `obj_sN/` 和一个 54 MB 的 `ExBarBrowser.xxx.exe`**。
验收脚本还会顺手建 `tN/`、`tmp_verify/` 之类的工作目录。这些都没有回收环节，
于是仓库根目录会稳定地以每次会话 150 MB+ 的速度膨胀：

    实测 2026-11-23：一个上午堆出 obj_rev2/s3/s4/s5/s6（5×101 MB）
    + 3 个 54 MB 的 exe，把当轮清掉的 607 MB 全部抵掉还倒涨。

清理靠人手 `rm -rf` 不行 —— 它会删掉正在跑的那份。所以判据必须是机器可判的。

## 判据（三条，任一命中就跳过）

  ① **实例在跑**       —— 同名的 `ExBarBrowser*.exe` 进程存在 → 那个 exe 不能删
  ② **最近有写入**     —— 目录树内最新 mtime 距今 < `--min-age`（默认 15 分钟）
                          → 它正在被生成/读取，跳过
  ③ **git 有跟踪**     —— 只要目录下有一项被 git 跟踪，整个目录不动

第 ② 条是核心：`obj_s6` 与 `obj_s5` 名字一样、大小一样，区别只在"10 分钟前还在写"。
不看 mtime 就会把别人的构建中间产物抽走，报出来的是莫名其妙的 undefined symbol。

## 分类

  BUILD_DIR    obj_*/ obj.*/            隔离构建的中间目录（不含主构建 obj/）
  SESSION_DIR  t<N>/ tmp_*/ tmpsfx*/ profile-*/ <PREFIX>_verify*   会话验收工作区
  EXE          ExBarBrowser*.exe        除主 ExBarBrowser.exe 外的冗余产物
  PROBE        tools/probe*.exe         已有同名 .cpp 的探针（可重编）
  PYCACHE      __pycache__/ *.pyc
  LOOSE        根目录 *.log / _h.json 等零散调试文件

## 用法

    python tools/reclaim.py                  # 只报告，不删（默认 dry-run）
    python tools/reclaim.py --apply          # 真删
    python tools/reclaim.py --apply --min-age 60    # 活跃保护放宽到 60 分钟
    python tools/reclaim.py --apply --quiet  # 只在有删除时输出

## 不许做

不递归删除仓库根以外的路径；不删 `obj/`、`src/`、`data/`、`docs/` 等白名单目录；
不删 git 跟踪的任何东西；不发任何窗口消息、不碰光标（R21）。
"""
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 永不触碰的顶层目录（含主构建 obj/ —— 回收它等于让下一轮全量重编）
KEEP_DIRS = {
    "obj", "src", "docs", "data", "tests", "tools", "scripts", "resources",
    "snapshots", "aichat", ".git", ".github", ".workbuddy",
    # 跨会话 obj 共享缓存（tools/objcache.py）。名字虽匹配 ^obj_.* 的 BUILD_DIR 规则，
    # 但它是加速资产不是残留——删掉只会让下轮全部重编。其陈旧条目由 objcache.prune() 自管。
    "obj_cache",
}

# 目录分类：名字 → 类别。顺序有意义，先匹配先定类。
DIR_RULES = [
    (re.compile(r"^obj_[^/]+$"), "BUILD_DIR"),    # obj_s3 / obj_audit / obj_rev2
    (re.compile(r"^obj\..+$"), "BUILD_DIR"),       # obj.xxx（.gitignore 里登记的写法）
    # t5 / t6 / t5d / t5s / t9 —— 会话验收工作区。带字母后缀也要认：
    # 实测有人用 t5d(daemon) / t5s(server) 区分同轮的两个实例，只认纯数字会漏一半。
    (re.compile(r"^t\d+[a-z]?$"), "SESSION_DIR"),
    (re.compile(r"^tmp.*$"), "SESSION_DIR"),       # tmp_ai_panel / tmpsfx2 / tmp_verify
    (re.compile(r"^profile-[^/]+$"), "SESSION_DIR"),
]

# 冗余 exe：ExBarBrowser*.exe 里除主产物外都算
EXE_KEEP = {"ExBarBrowser.exe"}
EXE_PAT = re.compile(r"^ExBarBrowser.*\.exe$", re.I)

# 根目录零散调试文件（不含 settings.json / bookmarks.html 这类真实配置）
LOOSE_FILES = {"audit.log", "debug.log", "_h.json", "out.txt"}

# 根目录的一次性会话脚本：t5.py / t6.py / t5d.py —— 与 tN/ 工作区配套。
# 目录被回收后它们就成了读不到东西的孤儿（脚本里写死 tN/ 路径）。
# 判据同样是「最近有没有被改」：正在调试的那份在保护期内，删不到。
LOOSE_SCRIPTS = re.compile(r"^t\d+[a-z]?\.py$")

# 根目录的临时截图：tmp_p.png / tmp_shot2.png。`.gitignore` 只挡了 shot*.png / out*.png，
# 手工命名的 tmp_*.png 会**进 git**（实测 2026-11-23 差点提交两张），所以由回收器兜住。
LOOSE_SHOTS = re.compile(r"^tmp.*\.(png|bmp)$", re.I)

PREFIX = "  "


def _fmt(nbytes):
    for unit in ("B", "K", "M", "G"):
        if nbytes < 1024 or unit == "G":
            return f"{nbytes:.0f}{unit}" if unit == "B" else f"{nbytes:.1f}{unit}"
        nbytes /= 1024.0


def _tree_size(path):
    """目录内所有普通文件的总字节数（跳过符号链接，避免环）。"""
    total = 0
    for dirpath, _dirnames, filenames in os.walk(path, followlinks=False):
        for f in filenames:
            try:
                total += os.lstat(os.path.join(dirpath, f)).st_size
            except OSError:
                pass
    return total


def _newest_mtime(path):
    """目录树内最新的 mtime；文件则取自身。用于判断"还在被写吗"。"""
    if path.is_file():
        try:
            return path.stat().st_mtime
        except OSError:
            return 0.0
    newest = 0.0
    for dirpath, _dirnames, filenames in os.walk(path, followlinks=False):
        try:
            newest = max(newest, os.lstat(dirpath).st_mtime)
        except OSError:
            pass
        for f in filenames:
            try:
                newest = max(newest, os.lstat(os.path.join(dirpath, f)).st_mtime)
            except OSError:
                pass
    return newest


# 构建进行中的标记文件，由 build_fast.py 在编译开始时写、结束时删。
#
# 【为什么光靠 mtime 不够】mtime 判据只能证明"最近写过"，不能证明"仍然持有"。
# 一个卡在某条 `/INCREMENTAL:NO` 上的构建（实测过：windres 遇到损坏的 .ico
# 会静默挂 20 分钟）期间不再写任何文件，于是窗口一过就被判定为陈旧 ——
# 回收器把它的 obj_sN 抽走，构建随后链接失败，报的是莫名其妙的 undefined symbol。
# 锁是显式的"我还在用"，比 mtime 的推断可靠。
LOCK_NAME = ".reclaim-lock"


def _pid_alive(pid):
    """判断 pid 是否真的还活着。**不能用 os.kill(pid, 0)**。

    实测（2026-11-23）：Windows 上 `os.kill(已退出pid, 0)` 直接返回成功，
    于是"崩溃后留下的陈旧锁"会被判成活锁，目录永久不被回收 —— 比不保护更糟。

    改用 OpenProcess + GetExitCodeProcess：
      OpenProcess 失败且 ERROR_INVALID_PARAMETER → pid 不存在
      GetExitCodeProcess 返回 STILL_ACTIVE          → 活着
      其余（权限不足等）                           → fail-closed，按活着处理

    判不清就保护：宁可多留 100 MB，也不要抽走别人正在构建的目录。
    """
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    ERROR_INVALID_PARAMETER = 87

    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    handle = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ctypes.get_last_error() != ERROR_INVALID_PARAMETER
    try:
        code = wintypes.DWORD()
        if not k32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True                       # 查不到退出码 → 判不清，按活着
        return code.value == STILL_ACTIVE
    finally:
        k32.CloseHandle(handle)


def active_locks():
    """返回 {相对路径: 持有者 pid}。空 dict = 当前没有构建在保护自己。"""
    held = {}
    try:
        entries = list(ROOT.glob(f"*/{LOCK_NAME}")) + list(ROOT.glob(LOCK_NAME))
    except OSError:
        return held
    for f in entries:
        try:
            owner = f.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        owner_pid = ""
        for tok in owner.replace("=", " ").split():
            if tok.isdigit():
                owner_pid = tok
                break
        if not owner_pid:
            # 锁文件内容认不出 pid：判不清，按活着处理（隔离构建崩在半路时常见）
            try:
                rel = f.parent.relative_to(ROOT).as_posix()
            except ValueError:
                continue
            held[rel] = "?"
            continue
        if _pid_alive(int(owner_pid)):
            try:
                rel = f.parent.relative_to(ROOT).as_posix()
            except ValueError:
                continue
            held[rel] = owner_pid
        # else: 持有者确证已退出 → 陈旧锁，不保护，让回收继续
    return held


def running_exe_names():
    """当前进程里所有 ExBarBrowser*.exe 的**小写文件名**。

    判据不用"能不能打开文件" —— Windows 允许读被占用的 exe，删才失败；
    而失败时机在 shutil.rmtree 里，会留下半删状态。先问进程表更干净。
    """
    names = set()
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq ExBarBrowser*", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=20,
            encoding="utf-8", errors="replace",
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return names
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith('"'):
            continue
        first = line.split('","')[0].strip('"').strip()
        if first:
            names.add(first.lower())
    return names


def git_tracked(paths):
    """返回这些路径中被 git 跟踪的集合（相对 ROOT）。

    目录用 `git ls-files -- <dir>` 问"底下有没有任何一项入库"。
    一次调用带全部路径，避免每个目录 fork 一次 git。
    """
    if not paths:
        return set()
    try:
        out = subprocess.run(
            ["git", "ls-files", "--"] + [str(p) for p in paths],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60,
            encoding="utf-8", errors="replace",
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return set()
    tracked_list = {ln.strip() for ln in out.splitlines() if ln.strip()}
    # 归到"命中的顶层条目"
    hit = set()
    for p in paths:
        rel = p.relative_to(ROOT).as_posix()
        if any(t == rel or t.startswith(rel + "/") for t in tracked_list):
            hit.add(rel)
    return hit


def collect(min_age_minutes):
    """产出 (类别, 路径, 大小, 跳过原因 or None) 列表。跳过原因非空 = 保留。"""
    now = time.time()
    age_limit = now - min_age_minutes * 60
    running = running_exe_names()

    # 先只收集"名字命中规则"的候选，再统一问 git，最后逐个判活跃度。
    candidates = []   # (cat, path)
    for entry in sorted(ROOT.iterdir()):
        name = entry.name
        if not entry.is_dir():
            if EXE_PAT.match(name) and name not in EXE_KEEP:
                candidates.append(("EXE", entry))
            elif name in LOOSE_FILES:
                candidates.append(("LOOSE", entry))
            elif LOOSE_SCRIPTS.match(name):
                candidates.append(("LOOSE", entry))
            elif LOOSE_SHOTS.match(name):
                candidates.append(("LOOSE", entry))
            continue
        if name in KEEP_DIRS:
            continue
        # __pycache__ 会出现在根目录（tools 之外的那份是跑 build_fast.py 时生成的）
        if name == "__pycache__":
            candidates.append(("PYCACHE", entry))
            continue
        for pat, cat in DIR_RULES:
            if pat.match(name):
                candidates.append((cat, entry))
                break

    # tools/probe*.exe 与 tools/debug.log 一类
    tools_dir = ROOT / "tools"
    if tools_dir.is_dir():
        for entry in sorted(tools_dir.iterdir()):
            if entry.is_dir():
                if entry.name == "__pycache__":
                    candidates.append(("PYCACHE", entry))
                continue
            if entry.suffix.lower() == ".exe":
                # 有同名 .cpp 才删（能重编）；没有说明是手工丢进来的，留着问人
                if (tools_dir / (entry.stem + ".cpp")).exists():
                    candidates.append(("PROBE", entry))
            elif entry.name in LOOSE_FILES:
                candidates.append(("LOOSE", entry))
            elif entry.suffix == ".pyc":
                candidates.append(("PYCACHE", entry))

    tracked = git_tracked([p for _c, p in candidates])
    locked = active_locks()          # {相对目录: pid}

    results = []
    for cat, path in candidates:
        rel = path.relative_to(ROOT).as_posix()
        size = _tree_size(path) if path.is_dir() else (path.stat().st_size if path.exists() else 0)
        reason = None

        if rel in tracked:
            reason = "git 有跟踪"
        elif rel in locked:
            reason = f"构建正在用（pid {locked[rel]}）"
        elif cat == "EXE" and path.name.lower() in running:
            reason = "实例在跑"
        elif _newest_mtime(path) > age_limit:
            # 时钟抖动可能让"刚写完"算出负数分钟，夹到 0 免得打印 "-0 分钟前"
            mins = max(0.0, (now - _newest_mtime(path)) / 60.0)
            reason = f"{mins:.0f} 分钟前还在写"
        results.append((cat, path, size, reason))

    return results


def sweep(min_age_minutes=15, apply=False, quiet=False, out=print):
    """回收一轮。返回 (释放字节, 删除条数, 保留条数)。

    这是给 `build_fast.py` 调的入口 —— 它只想要"回收了没有"，不关心清单。
    """
    results = collect(min_age_minutes)
    freed = 0
    removed = 0
    kept = 0
    by_cat = {}

    for cat, path, size, reason in results:
        if reason:
            kept += 1
            if not quiet:
                out(f"{PREFIX}保留 [{cat}] {path.relative_to(ROOT).as_posix()} —— {reason}")
            continue
        if not apply:
            freed += size
            removed += 1
            by_cat.setdefault(cat, [0, 0])
            by_cat[cat][0] += 1
            by_cat[cat][1] += size
            if not quiet:
                out(f"{PREFIX}将删 [{cat}] {path.relative_to(ROOT).as_posix()} ({_fmt(size)})")
            continue
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        except OSError as e:
            out(f"{PREFIX}删除失败 {path.relative_to(ROOT).as_posix()}：{e}")
            continue
        freed += size
        removed += 1
        by_cat.setdefault(cat, [0, 0])
        by_cat[cat][0] += 1
        by_cat[cat][1] += size
        if not quiet:
            out(f"{PREFIX}已删 [{cat}] {path.relative_to(ROOT).as_posix()} ({_fmt(size)})")

    return freed, removed, kept


def main():
    min_age = 15
    apply = "--apply" in sys.argv
    quiet = "--quiet" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--min-age" and i + 1 < len(sys.argv):
            try:
                min_age = int(sys.argv[i + 1])
            except ValueError:
                print("--min-age 需要整数（分钟）")
                return 2

    mode = "回收" if apply else "预演（加 --apply 才真删）"
    print(f"[RECLAIM] 模式={mode}  活跃保护={min_age} 分钟  根={ROOT}")
    freed, removed, kept = sweep(min_age, apply=apply, quiet=quiet)
    verb = "已释放" if apply else "可释放"
    print(f"[RECLAIM] {verb} {_fmt(freed)}，{'删除' if apply else '待删'} {removed} 项，保留 {kept} 项")
    if not apply and removed:
        print("[RECLAIM] 确认无误后跑：python tools/reclaim.py --apply")
    return 0


if __name__ == "__main__":
    sys.exit(main())