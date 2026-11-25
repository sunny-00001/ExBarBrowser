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
  python build_fast.py --release    # 发布构建：-O2 / 去 -g / -DNDEBUG / 链接期 strip

产物安全：默认先链接到 ExBarBrowser.new.exe，成功后原子替换 ExBarBrowser.exe。
  若 exe 正被运行中的进程占用而替换失败，保留 .new.exe 并提示，绝不破坏运行中的程序。
"""
import os
import re
import sys
import time
import atexit
import fnmatch
import argparse
import hashlib
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))   # objcache / reclaim 都在 tools/
CLANG = Path(r"C:\Program Files\Windhawk\Compiler\bin\clang++.exe")
LLD = Path(r"C:\Program Files\Windhawk\Compiler\bin\ld.lld.exe")
WINDRES = Path(r"D:\MinGW\bin\windres.exe")
SRC = ROOT / "src"
OBJ = ROOT / "obj"
RES = ROOT / "resources"
EXE = ROOT / "ExBarBrowser.exe"
NEW_EXE = ROOT / "ExBarBrowser.new.exe"
PCH_H = SRC / "common.h"
PCH = OBJ / "common.h.gch"
PCH_DEPS_SIG = OBJ / "common.h.gch.deps"
SIG = OBJ / ".build_sig"
RES_RC = RES / "app.rc"
RES_ICO = RES / "app.ico"
RES_OBJ = OBJ / "app.res.obj"
VERSION_FILE = ROOT / "VERSION"   # exe 版本资源与发布包的唯一版本号来源
VERSION_H = OBJ / "version.h"     # 由 VERSION 生成，构建时经 -I 传给 windres

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

# 发布构建：在 COMPILE_FLAGS 基础上把 -O1/-g 换成 -O2、去掉调试符号、定义 NDEBUG。
# 源码里没有 assert()（实测 0 处），所以 -DNDEBUG 不改变行为。用推导而非另抄一份，
# 避免两份清单漂移；链接期再加 -s 剥符号，两者组合把 exe 从约 60 MB 压到约 3 MB。
RELEASE_COMPILE_FLAGS = [
    ("-O2" if f == "-O1" else f) for f in COMPILE_FLAGS if f != "-g"
] + ["-DNDEBUG"]

LIBS = [
    "-static-libstdc++", "-static-libgcc",
    "-lole32", "-loleaut32", "-luuid", "-luser32", "-lgdi32", "-lshell32",
    "-lshlwapi", "-lcomctl32", "-ldwmapi", "-luxtheme", "-ladvapi32", "-lpropsys",
    "-luiautomationcore", "-lbcrypt",
    # 形态 A（D13）：AI 面板自己发 HTTPS 请求。WinHTTP 是 Windows 系统库，
    # 不新增任何运行时依赖（仍是单 exe、零安装）。
    # 密钥走 DPAPI（CryptProtectData / CryptUnprotectData），这两个在 Crypt32.dll。
    "-lwinhttp", "-lcrypt32",
    # AI 面板的 WebSocket 通道（aichat_webview.cpp）走 winsock：
    # socket/connect/htons/closesocket 都在 Ws2_32.dll。同样是系统库，零新增依赖。
    "-lws2_32",
]


def default_jobs():
    """自适应并行度：吃满 CPU，但高并发多会话时自动分摊，避免互抢核。

    【为什么不是固定 8】实测 16 核机器上冷编译 -j8=37.2s、-j16=30.7s，固定 8 白白
    浪费一半核。但高并发下若每个会话都吃满 16 线程，N 个会话就是 16N 个进程抢 16 核，
    上下文切换反而更慢。所以按"当前活跃构建数"把核数平分。

    活跃构建数取自各会话写在自己 obj 目录的 .reclaim-lock（take_lock 在 main 里写，
    这里数的是别的会话已经写下的），与回收器同一套判据，不另造状态。
    -j 显式指定时本函数不参与；EXBAR_JOBS 环境变量可整体钉住（CI / 排障用）。
    """
    env = os.environ.get("EXBAR_JOBS", "").strip()
    if env.isdigit() and int(env) > 0:
        return int(env)
    cpu = os.cpu_count() or 4
    cap = max(4, min(cpu, 16))              # 上限 16：再多核也别让单会话占满
    try:
        # 别的会话的锁：直接子目录(如 tmp_x/obj 下的两层)与 obj/ 本体
        locks = set(ROOT.glob("*/.reclaim-lock")) | set(ROOT.glob("*/*/.reclaim-lock"))
        active = len(locks)
    except OSError:
        active = 0
    return max(4, cap // max(1, active + 1))  # +1 = 本会话自己


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
    """PCH 是否需要重建。

    【为什么不能只看 mtime】只看 mtime 会漏掉一种情况：**内容变了但 mtime 没变新**。
    典型来源是 `git checkout` / 解压覆盖 / 某些同步工具 —— 它们把文件内容换掉，
    却留下一个比 PCH 更早的时间戳。于是 PCH 被判成"新鲜"，接着每个直接
    include 了 PCH 里那些头的 TU 都报重定义（"redefinition of ..."），
    而报错指向的是 str.h 这种**根本没被改过**的文件 —— 排查方向完全被带偏。

    2026-11-21 实测踩到：临时测试 obj 目录里的 PCH 是上一轮留下的，
    str.h 之后被换过内容但时间戳更早，于是整轮测试构建失败，报错却指向 str.h。

    所以这里改成**依赖指纹**：路径 + 内容哈希 + mtime_ns。内容哈希抓
    "内容变了但时间戳没变新"；mtime 抓 clang 自己的 PCH 校验口径
    （头文件被 touch 后内容相同也会拒绝旧 PCH）。两者缺一都会让整轮构建
    以"PCH 失效"告终，而报错指向无辜的 str.h。哈希 15 个小头文件不到 1ms，
    比重新生成 PCH（3~5s）便宜两个数量级。
    """
    if not use_pch:
        return False
    if not PCH.exists():
        return True
    if not PCH_DEPS_SIG.exists():
        return True
    try:
        want = PCH_DEPS_SIG.read_text(encoding="utf-8").strip()
    except OSError:
        return True
    return want != pch_deps_hash()


def pch_deps_hash():
    """PCH 依赖集的指纹（路径 + 内容 + mtime_ns，任一变化就重建）。"""
    h = hashlib.sha256()
    for f in sorted(pch_deps()):
        try:
            rel = f.relative_to(ROOT).as_posix()
        except ValueError:
            rel = str(f)
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        try:
            h.update(hashlib.sha256(f.read_bytes()).digest())
            h.update(str(f.stat().st_mtime_ns).encode("ascii"))
        except OSError:
            h.update(b"<missing>")
        h.update(b"\0")
    return h.hexdigest()


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
# ---------------------------------------------------------------------------
# 版本号：仓库根 VERSION 是单一真相源
# ---------------------------------------------------------------------------
def read_version():
    """返回 (版本字符串, major, minor, patch)；非法版本号直接中止构建。

    语义化版本 x.y.z（允许 -pre/+build 后缀，但 VERSIONINFO 的四个数字段
    只取 x.y.z，第四段固定 0，与 Windows 资源管理器显示习惯一致）。
    """
    raw = VERSION_FILE.read_text(encoding="utf-8").strip()
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.-]+)?", raw)
    if not m:
        raise RuntimeError(f"VERSION 格式非法（应为 x.y.z）：{raw!r} @ {VERSION_FILE}")
    major, minor, patch = (int(x) for x in m.groups())
    if max(major, minor, patch) > 65535:
        raise RuntimeError(f"VERSION 数值超出 VERSIONINFO 的 16 位字段：{raw!r}")
    return raw, major, minor, patch


def write_version_header():
    """从 VERSION 生成 obj/version.h；内容不变时不写，避免无谓触发资源重编。

    用头文件而不是 -D 传字符串宏：windres 会二次拼装 cpp 命令行，-D 里的引号
    在 Windows 上容易被吃掉/转义成 \\"，生成 rc 语法错误。头文件经 -I 引用没有这个问题。
    """
    version, major, minor, patch = read_version()
    text = "\n".join([
        "// 由 build_fast.py 从 VERSION 自动生成，请勿手改。",
        f"#define EXBAR_VERSION_MAJOR {major}",
        f"#define EXBAR_VERSION_MINOR {minor}",
        f"#define EXBAR_VERSION_PATCH {patch}",
        "#define EXBAR_VERSION_BUILD 0",
        f'#define EXBAR_VERSION_STR "{version}"',
        f'#define EXBAR_FILE_VERSION_STR "{version}.0"',
    ]) + "\n"
    VERSION_H.parent.mkdir(parents=True, exist_ok=True)
    if VERSION_H.exists() and VERSION_H.read_text(encoding="utf-8") == text:
        return version
    VERSION_H.write_text(text, encoding="utf-8")
    return version


def resource_needed(cur_sig):
    """ico/rc/VERSION 变动、签名变动或资源对象缺失时重编资源。"""
    if not WINDRES.exists():
        return False, ""  # 没 windres 就跳过，由调用方提示
    if not RES_OBJ.exists():
        return True, "无资源对象"
    if not SIG.exists() or SIG.read_text(encoding="utf-8").strip() != cur_sig:
        return True, "编译配置变化"
    obj_mt = RES_OBJ.stat().st_mtime_ns
    for src in (RES_RC, RES_ICO, VERSION_FILE, VERSION_H):
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
    if not VERSION_H.exists():
        raise RuntimeError("找不到版本头（应先调用 write_version_header）: " + str(VERSION_H))
    RES_OBJ.parent.mkdir(parents=True, exist_ok=True)
    # app.rc 通过 -I 找到 obj/version.h，版本号的唯一来源仍是仓库根 VERSION。
    # 只在构建注入 EXBAR_HAS_GENERATED_VERSION，手工单编 app.rc 时走 rc 内兜底值。
    cmd = [str(WINDRES), "-O", "coff", "-DEXBAR_HAS_GENERATED_VERSION=1",
           "-I", str(OBJ), "-i", str(RES_RC), "-o", str(RES_OBJ)]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        raise RuntimeError("图标资源编译失败:\n" + r.stderr[-1500:])
    return dt


def compile_one(src_rel, flags, use_pch, sig="", pch_hash=""):
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
    # 编译成功 → 回写共享缓存（失败不写；写缓存失败不影响构建结论）
    if sig:
        try:
            import objcache
            key = objcache.cache_key(SRC / Path(src_rel).name, sig, pch_hash, dep_path)
            objcache.store(key, obj_path, dep_path)
        except Exception:                                  # noqa: BLE001 —— 缓存是加速，不该影响构建
            pass
    return src_rel, True, dt, ""


# ---------------------------------------------------------------------------
# 链接 / 部署
# ---------------------------------------------------------------------------
def link_all(out_path, strip=False):
    objs = [str(obj_path_for(s)[0]) for s in SOURCES]
    # 图标资源对象（windres 编译的 COFF），随主程序一起链接进 exe。
    if RES_OBJ.exists():
        objs.append(str(RES_OBJ))
    # 链接必须显式带 -target：clang 本机默认是 i686，不带会与 x64 的 obj 冲突。
    cmd = [str(CLANG), "-mwindows", "-target", "x86_64-w64-mingw32"]
    # 发布构建在链接期剥离符号表/调试信息（-s 对 GNU ld 与 lld 都成立）。
    # 与 RELEASE_COMPILE_FLAGS 去 -g 组合：obj 里不带 DWARF，链接再剥符号 → 体积最小。
    if strip:
        cmd.append("-s")
    # lld 比 GNU ld 快约 40%（实测 0.5s -> 0.31s）。链接器不进配置签名——
    # 换链接器不改变目标代码语义，不该触发 165 个 TU 全量重编。
    if LLD.exists():
        cmd.append("-fuse-ld=lld")
    cmd += objs + ["-o", str(out_path)] + LIBS
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


def reclaim(min_age_minutes=15):
    """构建收尾回收：删掉会话残留的 obj_*/ tN/ 与冗余 exe。

    【为什么要挂在构建里】隔离构建（--objdir/--out）每轮留下 100 MB+ 的中间目录和
    一个 54 MB 的 exe，没有任何环节回收它们 —— 实测一个上午堆出 5 个 obj_sN 把
    当轮清掉的 607 MB 全抵掉还倒涨。人工 `rm -rf` 又不敢跑：它会抽走正在构建的那份。

    判据全在 tools/reclaim.py（唯一实现），这里只负责"每次构建末尾问一遍"。它自己会：
      - 跳过正在运行的 exe（按进程表）
      - 跳过 15 分钟内还在写的目录（这次构建自己写的那份天然被保护）
      - 跳过持有 .reclaim-lock 的目录（卡住的构建也能保护自己）
      - 跳过 git 跟踪的任何项
    所以挂在这里是安全的：本轮的产物刚写完，必定落在保护期内。

    失败绝不打断构建 —— 回收是保洁，不是验收项。
    """
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        # 注意模块名不能叫 gc —— 那是 Python 内建模块，import 只会拿到内建的，
        # 于是 sweep 不存在、回收静默失效（已实测踩过一次）。
        import reclaim as reclaimmod
        freed, removed, _kept = reclaimmod.sweep(min_age_minutes, apply=True, quiet=True,
                                                 out=lambda _m: None)
    except Exception as e:                              # noqa: BLE001 —— 保洁不许影响构建结论
        print(f"[回收] 跳过（{type(e).__name__}: {e}）", flush=True)
        return
    if removed:
        print(f"[回收] 清掉 {removed} 项历史残留，释放 {freed / 1048576:.1f} MB", flush=True)


def take_lock(lock_dir):
    """在中间目录写 .reclaim-lock，声明"这个目录正在被构建使用"。

    【为什么 mtime 不够】mtime 只证明"最近写过"，不证明"仍然持有"。一个卡住的
    构建（实测 windres 遇损坏 .ico 会静默挂 20 分钟）期间不写任何文件，窗口一过
    就被判成陈旧，回收器把它的中间目录抽走，链接期报成 undefined symbol。
    锁是显式的"我还在用"，比推断可靠。

    返回锁文件路径（供 release_lock 用）；写不进去返回 None —— 构建照常继续，
    只是失去显式保护（mtime 判据仍在）。
    """
    try:
        lock_dir.mkdir(parents=True, exist_ok=True)
        f = lock_dir / ".reclaim-lock"
        f.write_text(f"pid={os.getpid()}\nstarted={time.strftime('%Y-%m-%d %H:%M:%S')}\n",
                     encoding="utf-8")
        return f
    except OSError:
        return None


def release_lock(lock_file):
    """释放构建锁。放在 finally 里调 —— 编译失败/异常也必须放，否则锁成永久的。"""
    if lock_file is None:
        return
    try:
        lock_file.unlink()
    except OSError:
        pass


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="ExBarBrowser 增量构建（主编译入口）")
    ap.add_argument("-j", type=int, default=0,
                    help="并行编译数（默认 0=自适应：按 CPU 核数并参考并发会话数；"
                         "EXBAR_JOBS 可钉住）")
    ap.add_argument("--clean", action="store_true", help="清空 obj 后全量重建")
    ap.add_argument("--no-pch", action="store_true", help="禁用 PCH")
    ap.add_argument("--stats", action="store_true", help="打印各 TU 编译耗时")
    ap.add_argument("--out", help="输出 exe 路径（默认 ExBarBrowser.exe）")
    ap.add_argument("--objdir", help="中间目录（obj/pch/资源对象），默认 obj/；"
                                     "多会话并行时各用一套，避免互相覆盖 obj 与 PCH")
    ap.add_argument("--no-gc", action="store_true",
                    help="本次构建结束后不做残留回收（排障/复现残留问题时用）")
    ap.add_argument("--no-objcache", action="store_true",
                    help="禁用跨会话 obj 缓存（默认开启；命中即硬链接复用，省去重复编译）")
    ap.add_argument("--gc-age", type=int, default=15,
                    help="回收的活跃保护窗口（分钟，默认 15）：此时间内被写过的目录视为在用")
    ap.add_argument("--release", action="store_true",
                    help="发布构建：-O2 / 去 -g / -DNDEBUG / 链接期 strip；"
                         "默认输出到 ExBarBrowser.release.exe、独立 obj_release/，不碰调试 exe")
    args = ap.parse_args()
    if args.j <= 0:
        args.j = default_jobs()

    # 发布与调试用不同的编译标志 → 不同的配置签名，天然分缓存。
    # 再把发布构建的默认中间目录与输出名都隔开：同一棵树里调试/发布可并存，
    # 各自 --clean/增量互不踩踏，也不会替换掉用户正在跑的调试 exe。
    flags = RELEASE_COMPILE_FLAGS if args.release else COMPILE_FLAGS
    if args.release and not args.objdir:
        args.objdir = str(ROOT / "obj_release")
    if args.release and not args.out:
        args.out = str(ROOT / "ExBarBrowser.release.exe")

    if not CLANG.exists():
        print("找不到 clang:", CLANG)
        return 2

    # 中间目录隔离：两个会话共用 obj/ 时会互相把对方的 .obj 判成"过期/被改"，
    # 更糟的是 PCH 被一方重建、另一方正拿旧 PCH 编译。给每个会话一套即可。
    global OBJ, PCH, PCH_DEPS_SIG, SIG, RES_OBJ, VERSION_H
    if args.objdir:
        OBJ = Path(args.objdir).resolve()
        PCH = OBJ / "common.h.gch"
        PCH_DEPS_SIG = OBJ / "common.h.gch.deps"
        SIG = OBJ / ".build_sig"
        RES_OBJ = OBJ / "app.res.obj"
        VERSION_H = OBJ / "version.h"

    # 生成版本头（VERSION -> obj/version.h），随后资源编译会把它打进 exe。
    write_version_header()
    print(f"[清单] src/*.cpp 自动扫描：{len(SOURCES)} 个 TU（中间目录 {OBJ}）", flush=True)

    use_pch = not args.no_pch
    cur_sig = signature(flags, LIBS, use_pch)
    OBJ.mkdir(parents=True, exist_ok=True)

    # 持锁：从这一刻到进程退出，tools/reclaim.py 都不会回收本目录。
    # 用 atexit 而不是 try/finally —— main 里有多个 return 点，挨个包会漏掉一处，
    # 而漏掉的那处留着锁文件，回收器就永远跳过这个目录（比漏回收更糟）。
    _lock = take_lock(OBJ)
    atexit.register(release_lock, _lock)

    if args.clean:
        for p in OBJ.glob("*.obj"):
            p.unlink()
        for p in OBJ.glob("*.d"):
            p.unlink()
        for p in (PCH, PCH_DEPS_SIG, SIG, RES_OBJ):
            if p.exists():
                p.unlink()

    t_start = time.time()

    # 1) PCH
    if pch_needed(use_pch):
        print("[PCH] 生成预编译头 ...", flush=True)
        pch_dt = build_pch(flags)
        PCH_DEPS_SIG.write_text(pch_deps_hash(), encoding="utf-8")
        print(f"[PCH] 完成 {pch_dt:.1f}s ({PCH.stat().st_size / 1e6:.0f} MB)", flush=True)
    elif use_pch:
        print("[PCH] 复用已有 common.h.gch", flush=True)

    # 1.5) 图标资源（windres: app.rc + app.ico -> app.res.obj）
    if WINDRES.exists():
        need_res, res_why = resource_needed(cur_sig)
        if need_res:
            print(f"[资源] 编译图标 + 版本资源 ({res_why}) ...", flush=True)
            res_dt = build_resource()
            print(f"[资源] 完成 {res_dt:.1f}s -> {RES_OBJ.name}（版本 {read_version()[0]}）", flush=True)
        else:
            print("[资源] 复用已有 app.res.obj（图标 + 版本）", flush=True)
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

    # 3) 查共享缓存：命中的 TU 硬链接复用，只有未命中的才真正编译。
    #    缓存是加速手段，任何异常都降级为"没命中 → 走正常编译"，绝不影响构建结论。
    use_cache = not args.no_objcache
    pch_hash = ""
    if use_cache and use_pch:
        # 用 PCH 依赖源的哈希作语义代表，不用 PCH 二进制哈希：各会话独立生成的
        # common.h.gch 因内嵌路径/时间戳而二进制不同，但语义（声明集）相同。
        # 用二进制哈希会让每个会话的 key 都不同 → 缓存形同虚设（实测踩过）。
        try:
            pch_hash = pch_deps_hash()
        except Exception:                                  # noqa: BLE001
            use_cache = False

    compile_todo = list(todo)
    cache_hits = 0
    if use_cache and todo:
        try:
            import objcache
            still = []
            for s in todo:
                obj_path, dep_path = obj_path_for(s)
                key = objcache.cache_key(SRC / Path(s).name, cur_sig, pch_hash, dep_path)
                if objcache.lookup(key, obj_path, dep_path):
                    cache_hits += 1
                else:
                    still.append(s)
            compile_todo = still
        except Exception:                                  # noqa: BLE001 —— 缓存故障全量回退
            compile_todo = list(todo)
            cache_hits = 0
    if todo:
        print(f"[缓存] 命中 {cache_hits}/{len(todo)}，需真编译 {len(compile_todo)}", flush=True)

    # 4) 并行编译（仅未命中的 TU）
    timings = []
    failed = False
    if compile_todo:
        workers = max(1, min(args.j, len(compile_todo)))
        with ThreadPoolExecutor(max_workers=workers) as ex:
            futs = {ex.submit(compile_one, s, flags, use_pch, cur_sig, pch_hash): s
                    for s in compile_todo}
            for fut in as_completed(futs):
                s, ok, dt, err = fut.result()
                timings.append((s, dt))
                if not ok:
                    failed = True
                    print(f"[编译失败] {s}\n{err}", flush=True)
    if failed:
        print("编译失败，未链接。")
        # 失败构建同样留下上一轮的 obj_*/tN/ 残留，所以这里也回收一次。
        # 不用 return 提前退出，就是为了不在失败路径上再抄一遍回收调用。
        if not args.no_gc:
            reclaim(args.gc_age)
        return 1

    SIG.write_text(cur_sig, encoding="utf-8")

    # 4) 链接（--out 直接写目标路径；默认走暂存文件再原子替换）
    # 暂存名跟着 objdir 走：两个会话并行构建时，各自写各自的 .new.exe，
    # 不会一个刚链接完就被另一个覆盖（共用 obj/ 时这会变成随机结果）。
    custom_out = Path(args.out).resolve() if args.out else None
    staged = NEW_EXE if OBJ == ROOT / "obj" else OBJ.parent / f"ExBarBrowser.{OBJ.name}.exe"
    link_target = custom_out if custom_out else staged
    link_dt = link_all(link_target, strip=args.release)
    mode = "发布" if args.release else "调试"
    print(f"[链接] {mode} {link_dt:.1f}s -> {link_target.name}", flush=True)

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

    # 5) 收尾回收（见 reclaim 的说明：本轮自己的产物在保护期内，删不到它）
    if not args.no_gc:
        reclaim(args.gc_age)

    return 0


if __name__ == "__main__":
    sys.exit(main())
