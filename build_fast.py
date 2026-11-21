#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ExBarBrowser 主编译入口（Python 增量构建）
=========================================
这是 build.sh / build.bat 的唯一底层实现。日常编译、CI 全量构建都走这里。

相对旧版单命令全量编译（实测 88s）做的三件事：
  1. PCH 预编译头：common.h 聚合 11 个 Windows/shell 头 + STL，30 个 TU 各自重复解析。
     预编译成 common.h.gch 并在编译每个 TU 时注入，单 TU 实测 4.58s -> 2.64s。
  2. 分 TU 增量：每个 .cpp 独立编译成 obj/xxx.obj，用 -MMD 生成 .d 依赖文件；
     只有源码或它依赖的头文件发生变化时才重编。改一行 = 重编 1 个 TU。
     头文件依赖被精确追踪，不会出现"改了头文件忘了重编依赖方"的静默错版本。
  3. 并行：多 TU 用线程池并发编译（默认 8 路，-j 可调）。

清单唯一真相源：本文件顶部的 SOURCES / COMPILE_FLAGS / LIBS。
  新增 .cpp 或系统库只改这里一处；build.sh / build.bat 是薄包装，自动跟随。

用法（经 build.sh / build.bat 透传，也可直接调用）：
  python build_fast.py               # 增量构建（日常）
  python build_fast.py -j 16         # 指定并行数
  python build_fast.py --clean       # 清空 obj 全量重建（CI / 排障）
  python build_fast.py --no-pch      # 禁用 PCH
  python build_fast.py --stats       # 打印各 TU 编译耗时
  python build_fast.py --out <path>  # 输出到指定路径（不打断运行中的实例）

产物安全：默认先链接到 ExBarBrowser.new.exe，成功后原子替换 ExBarBrowser.exe。
  若 exe 正被运行中的进程占用而替换失败，保留 .new.exe 并提示，绝不破坏运行中的程序。
"""
import os
import re
import sys
import time
import fnmatch
import argparse
import hashlib
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CLANG = Path(r"C:\Program Files\Windhawk\Compiler\bin\clang++.exe")
WINDRES = Path(r"D:\MinGW\bin\windres.exe")
SRC = ROOT / "src"
OBJ = ROOT / "obj"
RES = ROOT / "resources"
EXE = ROOT / "ExBarBrowser.exe"
NEW_EXE = ROOT / "ExBarBrowser.new.exe"
PCH_H = SRC / "common.h"
PCH = OBJ / "common.h.gch"
SIG = OBJ / ".build_sig"
RES_RC = RES / "app.rc"
RES_ICO = RES / "app.ico"
RES_OBJ = OBJ / "app.res.obj"

# ---------------------------------------------------------------------------
# 源文件清单：自动扫描 src/*.cpp，只需维护排除表
#
# 为什么不再手写清单：多人/多会话并行时，新增一个 .cpp 就要在同一段字符串里
# 插一行 —— 这是"必冲突"的改动，还会因为冲突解决不当而静默漏文件（直到链接期
# 才报未定义符号）。自动扫描把这类冲突降到零，语义仍然确定：
#   包含 src/*.cpp；排除下面 EXCLUDE_PATTERNS 命中的名字。
# ---------------------------------------------------------------------------
EXCLUDE_PATTERNS = [
    "*.bak.cpp", "*.orig.cpp",          # 手工备份
    "tmp_*", "*_scratch*", "_*",        # 临时试验文件（下划线开头 = 不参与构建）
]


def discover_sources():
    """按排除表收集 src/*.cpp，返回相对 ROOT 的 posix 路径（已排序）。"""
    out = []
    for p in sorted(SRC.glob("*.cpp")):
        name = p.name
        if any(fnmatch.fnmatch(name, pat) for pat in EXCLUDE_PATTERNS):
            continue
        out.append(f"src/{name}")
    return out


SOURCES = discover_sources()

COMPILE_FLAGS = [
    "-std=c++20", "-O1", "-g", "-target", "x86_64-w64-mingw32",
    "-DUNICODE", "-D_UNICODE", "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00",
    "-D_WIN32_IE=0x0A00", "-DNTDDI_VERSION=0x0A000008", "-Isrc",
    # 历史代码里有一些"未使用参数/字段初始化"噪音，先只开高价值警告组。
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    # -Wshadow / -Wconversion 于 2026-11-21 清零后开启（当时 53 -> 0）。
    # 开着的意义是**让新警告立刻可见**：原来 53 条里大多无害，但真正危险的
    # 那一条会被淹没。这两组是本次清理中唯一抓到过真实隐患的：
    #   · shadow -> 内外层同名导致"看着是同一个量"的误读
    #   · conversion -> int 下标传给收 WPARAM 的 ListBox 消息
    "-Wshadow", "-Wconversion",
]

LIBS = [
    "-static-libstdc++", "-static-libgcc",
    "-lole32", "-loleaut32", "-luuid", "-luser32", "-lgdi32", "-lshell32",
    "-lshlwapi", "-lcomctl32", "-ldwmapi", "-luxtheme", "-ladvapi32", "-lpropsys",
    "-luiautomationcore",
]


def signature(flags, libs, use_pch):
    """配置指纹；任一编译选项 / 库 / PCH 开关 / 编译器变化 => 全部重编。"""
    try:
        tool = subprocess.run([str(CLANG), "--version"], capture_output=True, text=True,
                              timeout=10).stdout.splitlines()[0]
    except Exception:
        tool = str(CLANG)
    raw = " ".join(flags + libs) + "|pch=" + str(use_pch) + "|cc=" + tool
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# PCH
# ---------------------------------------------------------------------------
def pch_deps():
    """common.h 递归包含的本地头（PCH 的真正依赖集）。

    以前只比较 common.h 自己的 mtime —— 改 str.h / log.h 这类被 common.h
    包含的子头时 PCH 不会重建，于是所有 TU 报
    "file has been modified since the precompiled header was built"。
    这不只是麻烦：CI 上会变成随机失败，而且报错信息指向的不是真正改过的文件。
    """
    seen = set()
    stack = [PCH_H]
    while stack:
        f = stack.pop()
        if f in seen or not f.exists():
            continue
        seen.add(f)
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
            stack.append(SRC / m.group(1))
    return seen


def pch_needed(use_pch):
    if not use_pch:
        return False
    if not PCH.exists():
        return True
    pch_mtime = PCH.stat().st_mtime_ns
    return any(h.stat().st_mtime_ns > pch_mtime for h in pch_deps() if h.exists())


def build_pch(flags):
    PCH.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG)] + flags + ["-x", "c++-header", str(PCH_H), "-o", str(PCH)]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        raise RuntimeError("PCH 生成失败:\n" + r.stderr[-2000:])
    return dt


# ---------------------------------------------------------------------------
# 依赖解析（clang -MMD 生成的 .d 文件）
# ---------------------------------------------------------------------------
def parse_dep(d_path):
    try:
        text = d_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    text = text.replace("\\\n", " ")  # 合并续行
    # target 分隔符是 ".obj:"（冒号紧跟空白）；不能用 find(":"),
    # 因为 Windows 盘符 "D:" 也含冒号。
    m = re.search(r":\s", text)
    body = text[m.end():] if m else text
    return [x for x in body.split() if x]


# ---------------------------------------------------------------------------
# 单个 TU 是否需要重编
# ---------------------------------------------------------------------------
def need_rebuild(src_rel, obj_path, dep_path, cur_sig, use_pch):
    if not obj_path.exists() or not dep_path.exists():
        return True, "无 obj/dep"
    if not SIG.exists() or SIG.read_text(encoding="utf-8").strip() != cur_sig:
        return True, "编译配置变化"
    if use_pch and PCH.exists() and PCH.stat().st_mtime_ns > obj_path.stat().st_mtime_ns:
        return True, "PCH 更新"
    obj_mt = obj_path.stat().st_mtime_ns
    deps = parse_dep(dep_path)
    if deps is None:
        return True, "依赖解析失败"
    for d in deps:
        p = Path(d)
        if not p.is_absolute():
            p = ROOT / d
        try:
            if p.stat().st_mtime_ns > obj_mt:
                return True, "依赖变更: " + p.name
        except OSError:
            return True, "依赖缺失: " + p.name
    return False, ""


def obj_path_for(src_rel):
    stem = Path(src_rel).stem
    return OBJ / (stem + ".obj"), OBJ / (stem + ".d")


# ---------------------------------------------------------------------------
# 图标资源（windres: .rc + .ico -> COFF 对象）
# ---------------------------------------------------------------------------
def resource_needed(cur_sig):
    """ico/rc 变动、签名变动或资源对象缺失时重编资源。"""
    if not WINDRES.exists():
        return False, ""  # 没 windres 就跳过，由调用方提示
    if not RES_OBJ.exists():
        return True, "无资源对象"
    if not SIG.exists() or SIG.read_text(encoding="utf-8").strip() != cur_sig:
        return True, "编译配置变化"
    obj_mt = RES_OBJ.stat().st_mtime_ns
    for src in (RES_RC, RES_ICO):
        if not src.exists():
            return True, "资源文件缺失: " + src.name
        if src.stat().st_mtime_ns > obj_mt:
            return True, "资源变更: " + src.name
    return False, ""


def build_resource():
    if not WINDRES.exists():
        raise RuntimeError("找不到 windres: " + str(WINDRES))
    if not RES_RC.exists():
        raise RuntimeError("找不到资源脚本: " + str(RES_RC))
    RES_OBJ.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(WINDRES), "-O", "coff", "-i", str(RES_RC), "-o", str(RES_OBJ)]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        raise RuntimeError("图标资源编译失败:\n" + r.stderr[-1500:])
    return dt


def compile_one(src_rel, flags, use_pch):
    obj_path, dep_path = obj_path_for(src_rel)
    obj_path.parent.mkdir(parents=True, exist_ok=True)
    cflags = list(flags) + ["-MMD", "-MF", str(dep_path)]
    if use_pch:
        cflags += ["-include-pch", str(PCH)]
    cmd = [str(CLANG)] + cflags + ["-c", str(SRC / Path(src_rel).name), "-o", str(obj_path)]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        return src_rel, False, dt, r.stderr[-1500:]
    return src_rel, True, dt, ""


# ---------------------------------------------------------------------------
# 链接 / 部署
# ---------------------------------------------------------------------------
def link_all(out_path):
    objs = [str(obj_path_for(s)[0]) for s in SOURCES]
    # 图标资源对象（windres 编译的 COFF），随主程序一起链接进 exe。
    if RES_OBJ.exists():
        objs.append(str(RES_OBJ))
    # 链接必须显式带 -target：clang 本机默认是 i686，不带会与 x64 的 obj 冲突。
    cmd = [str(CLANG), "-mwindows", "-target", "x86_64-w64-mingw32"] + objs + \
          ["-o", str(out_path)] + LIBS
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        raise RuntimeError("链接失败:\n" + r.stderr[-2500:])
    return dt


def deploy(staged=None):
    """原子替换目标 exe；占用时保留暂存文件并提示。"""
    try:
        os.replace(staged or NEW_EXE, EXE)
        return True, ""
    except OSError as e:
        return False, str(e)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="ExBarBrowser 增量构建（主编译入口）")
    ap.add_argument("-j", type=int, default=8, help="并行编译数（默认 8）")
    ap.add_argument("--clean", action="store_true", help="清空 obj 后全量重建")
    ap.add_argument("--no-pch", action="store_true", help="禁用 PCH")
    ap.add_argument("--stats", action="store_true", help="打印各 TU 编译耗时")
    ap.add_argument("--out", help="输出 exe 路径（默认 ExBarBrowser.exe）")
    ap.add_argument("--objdir", help="中间目录（obj/pch/资源对象），默认 obj/；"
                                     "多会话并行时各用一套，避免互相覆盖 obj 与 PCH")
    args = ap.parse_args()

    if not CLANG.exists():
        print("找不到 clang:", CLANG)
        return 2

    # 中间目录隔离：两个会话共用 obj/ 时会互相把对方的 .obj 判成"过期/被改"，
    # 更糟的是 PCH 被一方重建、另一方正拿旧 PCH 编译。给每个会话一套即可。
    global OBJ, PCH, SIG, RES_OBJ
    if args.objdir:
        OBJ = Path(args.objdir).resolve()
        PCH = OBJ / "common.h.gch"
        SIG = OBJ / ".build_sig"
        RES_OBJ = OBJ / "app.res.obj"

    print(f"[清单] src/*.cpp 自动扫描：{len(SOURCES)} 个 TU（中间目录 {OBJ}）", flush=True)

    use_pch = not args.no_pch
    cur_sig = signature(COMPILE_FLAGS, LIBS, use_pch)
    OBJ.mkdir(parents=True, exist_ok=True)

    if args.clean:
        for p in OBJ.glob("*.obj"):
            p.unlink()
        for p in OBJ.glob("*.d"):
            p.unlink()
        for p in (PCH, SIG, RES_OBJ):
            if p.exists():
                p.unlink()

    t_start = time.time()

    # 1) PCH
    if pch_needed(use_pch):
        print("[PCH] 生成预编译头 ...", flush=True)
        pch_dt = build_pch(COMPILE_FLAGS)
        print(f"[PCH] 完成 {pch_dt:.1f}s ({PCH.stat().st_size / 1e6:.0f} MB)", flush=True)
    elif use_pch:
        print("[PCH] 复用已有 common.h.gch", flush=True)

    # 1.5) 图标资源（windres: app.rc + app.ico -> app.res.obj）
    if WINDRES.exists():
        need_res, res_why = resource_needed(cur_sig)
        if need_res:
            print(f"[资源] 编译图标资源 ({res_why}) ...", flush=True)
            res_dt = build_resource()
            print(f"[资源] 完成 {res_dt:.1f}s -> {RES_OBJ.name}", flush=True)
        else:
            print("[资源] 复用已有 app.res.obj", flush=True)
    else:
        print("[资源] 跳过（未找到 windres，exe 将使用系统默认图标）", flush=True)

    # 2) 判定需要重编的 TU
    todo = []
    skipped = 0
    reasons = {}
    for s in SOURCES:
        obj_path, dep_path = obj_path_for(s)
        need, why = need_rebuild(s, obj_path, dep_path, cur_sig, use_pch)
        if need:
            todo.append(s)
            reasons[s] = why
        else:
            skipped += 1
    print(f"[增量] 需重编 {len(todo)}/{len(SOURCES)}，跳过 {skipped}", flush=True)
    if todo:
        for s in todo[:6]:
            print(f"        - {Path(s).name}  ({reasons[s]})")
        if len(todo) > 6:
            print(f"        ... 还有 {len(todo) - 6} 个")

    # 3) 并行编译
    timings = []
    failed = False
    if todo:
        workers = max(1, min(args.j, len(todo)))
        with ThreadPoolExecutor(max_workers=workers) as ex:
            futs = {ex.submit(compile_one, s, COMPILE_FLAGS, use_pch): s for s in todo}
            for fut in as_completed(futs):
                s, ok, dt, err = fut.result()
                timings.append((s, dt))
                if not ok:
                    failed = True
                    print(f"[编译失败] {s}\n{err}", flush=True)
    if failed:
        print("编译失败，未链接。")
        return 1

    SIG.write_text(cur_sig, encoding="utf-8")

    # 4) 链接（--out 直接写目标路径；默认走暂存文件再原子替换）
    # 暂存名跟着 objdir 走：两个会话并行构建时，各自写各自的 .new.exe，
    # 不会一个刚链接完就被另一个覆盖（共用 obj/ 时这会变成随机结果）。
    custom_out = Path(args.out).resolve() if args.out else None
    staged = NEW_EXE if OBJ == ROOT / "obj" else OBJ.parent / f"ExBarBrowser.{OBJ.name}.exe"
    link_target = custom_out if custom_out else staged
    link_dt = link_all(link_target)
    print(f"[链接] {link_dt:.1f}s -> {link_target.name}", flush=True)

    total = time.time() - t_start
    if custom_out:
        print(f"[完成] 总耗时 {total:.1f}s -> {custom_out}")
    else:
        ok, err = deploy(link_target)
        if ok:
            print(f"[完成] 总耗时 {total:.1f}s -> {EXE.name}")
        else:
            print(f"[完成] 总耗时 {total:.1f}s，但目标 exe 被占用，无法替换。")
            print(f"        新版本已保存为 {link_target.name}，关闭程序后手动替换或重跑。")
            print(f"        原因: {err}")

    if args.stats:
        print("\n[各 TU 耗时]")
        for s, dt in sorted(timings, key=lambda x: -x[1]):
            print(f"  {dt:5.2f}s  {Path(s).name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
