#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跨会话共享 obj 缓存：同一份源码+配置只编译一次，其余会话硬链接复用。

## 为什么需要它

高并发开发时多个会话各用独立 --objdir（物理隔离是对的，避免互相抽 obj），
代价是**同一批 165 个 TU 被每个会话各编一遍**。实测冷编译 -j16 要 30.7s，
4 个会话就是 4×30.7s 的 CPU + 4×100MB 的 obj。而这些 obj 的内容完全一样
（同一份源码、同一套 flags、同一个 PCH）——重复编译纯属浪费。

内容寻址缓存把"编译结果"按 (源内容, 配置签名, PCH 内容) 存进工程内 obj_cache/：
后到的会话发现键已存在，直接硬链接到自己的 objdir，跳过编译。命中一份 obj
的成本 ≈ 0（一次哈希 + 一次 link），而重编要 1~3s。

## 为什么安全

- **键包含全部输入**：源码内容、编译 flags+库签名、PCH 依赖内容、以及该 TU
  **全部头文件的内容指纹**（由 .d 清单逐文件哈希，见 dep_fingerprint）。
  任一变化键就变，绝不会拿到过期 obj（比 mtime 判据可靠：内容哈希不看时间戳）。
  2026-11-24 的 ABI 混链事故正是因为早期键漏掉了头文件内容——已补齐。
- **清缓存不是常规手段**：键已能自证新旧，日常建包不清池（保留加速）。
  仅在怀疑缓存被外部污染时手动 `python tools/objcache.py`，或全量构建加
  `--clean --no-objcache`。这两个都是兜底，不进 build.sh 的常规路径。
- **硬链接而非复制**：objdir 与缓存指向同一物理文件，零额外磁盘；某会话删自己
  objdir 的链接不影响缓存，缓存清空也不影响已链接的 obj（链接计数）。
- **内容相同则结果相同**：clang 编译是纯函数（同输入同输出），不存在"这个 obj
  必须是这个会话专属"的情况。调试信息里的源路径都是绝对路径，各会话一致。
- **并发写安全**：先写临时文件再 os.replace 原子落键；两个会话同时算同一键，
  内容相同，后写覆盖先写无害。

## 边界

- 缓存池 obj_cache/ 在 tools/reclaim.py 的 KEEP_DIRS 里（不能当残留回收）。
- 键目录名是哈希，无业务语义；缓存条目用 mtime 判陈旧，见 prune()。
- 不发任何窗口消息、不碰光标（R21）；纯文件操作。
"""
import os
import re
import time
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "obj_cache"

# 与 build_fast 一致：.d 文件里的依赖解析（合并续行后取冒号之后的部分）。
# Windows 盘符含冒号，所以认 ".obj:" 而不是第一个冒号。
def _dep_targets(d_text):
    text = d_text.replace("\\\n", " ")
    m = re.search(r":\s", text)
    body = text[m.end():] if m else text
    return [x for x in body.split() if x]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# 同一轮构建里同一个头文件会被几百个 TU 重复哈希，按 (路径,mtime,大小) 缓存一下。
_HASH_CACHE = {}


def _sha256_cached(path):
    p = Path(path)
    try:
        st = p.stat()
    except OSError:
        return None
    ck = (str(p), st.st_mtime_ns, st.st_size)
    v = _HASH_CACHE.get(ck)
    if v is None:
        v = sha256_file(p)
        _HASH_CACHE[ck] = v
    return v


def dep_fingerprint(dep_path):
    """该 TU 的全部依赖（.d 清单）内容指纹。无 .d → None（不能安全命中缓存）。"""
    p = Path(dep_path) if dep_path else None
    if not p or not p.exists():
        return None
    try:
        text = p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    h = hashlib.sha256()
    for d in sorted(set(_dep_targets(text))):
        q = Path(d)
        if not q.is_absolute():
            q = ROOT / d
        digest = _sha256_cached(q)
        h.update(d.encode("utf-8", "replace"))
        h.update(b"\0")
        h.update((digest or "<missing>").encode("ascii"))
        h.update(b"\0")
    return h.hexdigest()


def cache_key(src_path, sig, pch_hash, dep_path=None):
    """缓存键：源内容 + 配置签名 + PCH 依赖 + 本 TU 全部头文件内容。

    【为什么必须带头文件内容 —— 2026-11-24 实测踩到】
    `nav_pane_root.h` 的 RootSpec 加了 `pidl` 成员，sizeof 从 0x40 变 0x58；
    .cpp 没改、flags 没改、PCH 依赖没改，旧键原样命中，缓存把"旧布局"编译的
    obj 覆盖回 objdir。`need_rebuild()` 明明判出"依赖变更"，却被缓存命中短路。
    结果是新旧 ABI 混链：后台 DiscoverExplorerRoots() 在 vector 搬迁时读野指针，
    ExBarBrowser 启动即 0xC0000005 / 0xC0000374，且全量 --no-objcache 重建即好。
    键里加入本 TU 的头文件内容后，头文件一变键必变，这类幽灵故障不再可能。
    """
    fp = dep_fingerprint(dep_path) if dep_path else None
    if fp is None:
        # 没有 .d 就无法证明 obj 与当前头文件一致。返回一个永不命中的键，
        # 强制真编译（编译完成后 store() 会带着新 .d 写入正常键）。
        return "nodep-" + hashlib.sha256(str(time.time_ns()).encode("ascii")).hexdigest()[:32]
    h = hashlib.sha256()
    h.update(sha256_file(src_path).encode("ascii"))
    h.update(b"\0")
    h.update(sig.encode("utf-8"))
    h.update(b"\0")
    h.update((pch_hash or "nopch").encode("ascii"))
    h.update(b"\0")
    h.update(fp.encode("ascii"))
    return h.hexdigest()[:32]


def _link_or_copy(src, dst):
    """优先硬链接（零拷贝），失败（跨卷/权限）则回退复制。"""
    try:
        os.link(src, dst)
        return "link"
    except OSError:
        import shutil
        shutil.copy2(src, dst)
        return "copy"


def _replace_with(src, dst):
    """用缓存条目原子覆盖目标位置；调用方已判定该 TU 需要重建。"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".cache.tmp")
    if tmp.exists() or tmp.is_symlink():
        tmp.unlink()
    _link_or_copy(src, tmp)
    os.replace(tmp, dst)


def lookup(key, obj_path, dep_path):
    """命中缓存则把 obj/dep 覆盖到目标位置，返回 True；否则 False。

    调用方（build_fast）只在 need_rebuild 为真时调用，因此这里**必须**覆盖
    已存在的旧 obj/dep。早期版本只在目标不存在时链接，导致源码改完后仍用
    旧 obj 链接，出现过 "源码已无重复定义、exe 却仍报 duplicate symbol" 的
    幽灵故障。
    """
    c_obj = CACHE / (key + ".obj")
    c_dep = CACHE / (key + ".d")
    if not (c_obj.exists() and c_dep.exists()):
        return False
    _replace_with(c_obj, obj_path)
    _replace_with(c_dep, dep_path)
    return True


def store(key, obj_path, dep_path):
    """把新编好的 obj/dep 原子写进缓存池。"""
    CACHE.mkdir(parents=True, exist_ok=True)
    for src, suffix in ((obj_path, ".obj"), (dep_path, ".d")):
        if not src.exists():
            continue
        final = CACHE / (key + suffix)
        if final.exists():
            continue                      # 内容相同的键已存在，不覆盖
        tmp = CACHE / (key + suffix + ".tmp")
        _link_or_copy(src, tmp)
        try:
            os.replace(tmp, final)
        except OSError:
            try:
                tmp.unlink()
            except OSError:
                pass


def stats():
    """返回 (条目数, 占用字节)。"""
    if not CACHE.exists():
        return 0, 0
    n = 0
    total = 0
    for p in CACHE.iterdir():
        if p.suffix in (".obj", ".d") and p.is_file():
            n += 1
            total += p.stat().st_size
    return n, total


def clear():
    """清空共享缓存池；只删缓存条目，不碰 obj/ 里的构建产物。返回删除文件数。

    建包前调用它，是为了杜绝"旧布局 obj 从缓存覆盖回 objdir"这类混链故障。
    与 lookup() 的头文件指纹是两道独立防线：指纹防"键没变内容变了"，
    清缓存防"任何历史遗留条目被复用"。硬链接的中间产物不受影响
    （unlink 缓存里的一条链接，objdir 里的那份仍然有效）。
    """
    if not CACHE.exists():
        return 0
    removed = 0
    for p in CACHE.iterdir():
        if p.suffix in (".obj", ".d", ".tmp") and p.is_file():
            try:
                p.unlink()
                removed += 1
            except OSError:
                pass
    return removed


def prune(max_age_days=14):
    """删掉超过 max_age_days 未访问的条目（obj 与 .d 成对删）。按 mtime 判据。

    与 reclaim 同一哲学：缓存是加速手段不是资产，陈旧条目删掉只是让下次重编，
    不会引起错误。默认 14 天，远高于正常迭代周期。返回删除的键数。
    """
    if not CACHE.exists():
        return 0
    cutoff = time.time() - max_age_days * 86400
    keys = set()
    for p in CACHE.iterdir():
        if p.suffix in (".obj", ".d") and p.is_file():
            keys.add(p.name[:-len(p.suffix)])
    removed = 0
    for key in keys:
        files = [CACHE / (key + ".obj"), CACHE / (key + ".d")]
        if all(f.exists() and f.stat().st_mtime < cutoff for f in files):
            for f in files:
                try:
                    f.unlink()
                except OSError:
                    pass
            removed += 1
    return removed


def main():
    """手动兜底：清空缓存池。常规建包不调用（键已能识别新旧，无需清池）。

    用法：怀疑缓存被外部污染时 `python tools/objcache.py` 清一次；
    之后正常 build.sh 会真编译受影响的 TU 并重建缓存。
    """
    n = clear()
    size = 0
    if CACHE.exists():
        for p in CACHE.iterdir():
            try:
                size += p.stat().st_size
            except OSError:
                pass
    print(f"[objcache] 已清空共享缓存：{n} 个文件（残留 {size / 1048576:.1f} MB）")


if __name__ == "__main__":
    main()
