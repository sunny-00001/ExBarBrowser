#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""评估面清单的签发与校验。

评估面 = 「判定代码对不对」的那套东西：构建器、测试入口、测试源、静态校验器。
Agent 自我迭代的闭环是「改代码 → 编译 → 跑测试 → 看结果」，而测试与构建脚本
本身就是判据。判据被改软之后，"测试通过"就不再说明代码是对的 ——
Darwin Gödel Machine 论文把这叫 objective hacking，并把"如何防止模型改
evaluator"列为公开问题。

本脚本做两件事：

    python tools/gen_eval_baseline.py            # 签发（覆盖 data/eval-surface.json）
    python tools/gen_eval_baseline.py --check    # 校验清单是否与当前仓库一致

规则是「只许签发，不许自改」：
  · 受保护文件被修改或删除 → 清单过期；
  · 受保护目录里**新增**文件 → 同样算过期（tests/ 下的 .cpp 会被编译并执行，
    新增一个就是新增一段会跑起来的代码）。

新增测试文件本身是好事（只会更强），但它必须由人显式签发一次 ——
这样"谁在什么时候放宽了判据"在 git 里留得下痕迹。

清单路径：<repo>/data/eval-surface.json（入库，因为它属于工程资产而不是用户数据）。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, "data", "eval-surface.json")

# 单个文件（仓库相对路径 → 用途说明）
#
# 【为什么清单本身**不在**这里】会形成循环：签发时要算清单的哈希，
# 而算完之后又要写进清单 —— 写进去的哈希立刻过期。
# 清单自身的完整性用另外两层解决（两者缺一不可）：
#
#   selfSeal   —— 对清单正文（除 selfSeal 外）做 SHA-256。防的是"手改清单"：
#                 直接把某条测试的哈希改掉而不重签，seal 立刻对不上。
#   git 跟踪   —— 清单必须已被 git 跟踪且工作树干净。防的是"重签"：
#                 重签会改变清单内容，于是它以"未提交的改动"出现，
#                 `git status` 与 CI 都看得见 —— 谁在什么时候放宽了判据，留痕。
#
# 这两层合起来才挡得住"改软判据 + 同步重签"这条路；单独任何一层都挡不住。
PROTECTED_FILES = {
    "build_fast.py": "构建器：编译与链接的唯一实现",
    "build.sh": "构建脚本（build_fast.py 的薄壳）",
    "tests/run_all.sh": "测试入口：枚举并运行全部单测",
}

# 受保护目录：目录里的文件集合必须与清单完全一致。
# ext 是纳入清单的文件扩展名。
PROTECTED_DIRS = {
    "tests": (".cpp", "测试源：会被编译并执行，是判据的一部分"),
    "tools": (".py", "静态校验器与工具脚本：判定命令表与实现是否一致"),
}


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def collect():
    """返回 (files, dirSets)：当前仓库的评估面现状。

    files 里既有单文件，也有受保护目录里的每一个文件 ——
    内容改动要靠哈希发现，新增/删除要靠 dirSets 的文件集合发现。
    两者缺一不可：只看哈希发现不了"新增了一个会跑起来的测试"，
    只看集合发现不了"把某个测试改空了"。
    """
    files = {}
    for rel in sorted(PROTECTED_FILES):
        abs_path = os.path.join(REPO, rel)
        if not os.path.isfile(abs_path):
            raise SystemExit(f"受保护文件不存在，无法签发清单：{rel}")
        files[rel] = sha256(abs_path)

    dir_sets = {}
    for rel_dir, (ext, _why) in sorted(PROTECTED_DIRS.items()):
        abs_dir = os.path.join(REPO, rel_dir)
        if not os.path.isdir(abs_dir):
            raise SystemExit(f"受保护目录不存在，无法签发清单：{rel_dir}")
        names = sorted(
            n for n in os.listdir(abs_dir)
            if n.lower().endswith(ext) and os.path.isfile(os.path.join(abs_dir, n))
        )
        dir_sets[rel_dir] = {"ext": ext, "files": names}
        for name in names:
            files[f"{rel_dir}/{name}"] = sha256(os.path.join(abs_dir, name))
    return files, dir_sets


def _canonical(obj) -> bytes:
    """清单正文的规范形式（用于 selfSeal）。

    【为什么不用 json.dumps】selfSeal 要能在 **C++ 侧**（dev.build 的门）
    独立复算出来，而 C++ 的 json 序列化与 Python 的缩进/转义/浮点格式
    不可能逐字节一致。一旦两边算出的哈希不同，这道门就会永远拒绝构建,
    或者更糟 —— 为让它通过而把校验删掉。

    所以规范形式只由 **纯 ASCII 的确定行** 构成，两侧都能一模一样地生成：
        V <version>
        F <相对路径> <sha256>
        D <目录> <扩展名> <文件1,文件2,...>      （文件列表已排序）
    键一律排序，分隔符是单个空格，行以 \\n 结尾，整体 UTF-8。
    路径与哈希都是 ASCII（Win32 路径由仓库相对路径给出；中文文件名会被
    写成 UTF-8 字节，两侧同样处理）。
    """
    lines = [f"V {obj.get('version', 0)}"]
    for rel in sorted(obj.get("files", {})):
        lines.append(f"F {rel} {obj['files'][rel]}")
    for d in sorted(obj.get("dirSets", {})):
        entry = obj["dirSets"][d]
        names = ",".join(sorted(entry.get("files", [])))
        lines.append(f"D {d} {entry.get('ext', '')} {names}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def compute_seal(manifest: dict) -> str:
    """对清单正文（除 selfSeal 本身）算 SHA-256。

    防的是**手改清单**：不重签却把某个文件的哈希改成新值。此时 seal 对不上。
    它防不住"用本脚本重签" —— 那个由 require_git_tracked 与 git diff 负责。
    """
    body = {k: v for k, v in manifest.items() if k != "selfSeal"}
    return hashlib.sha256(_canonical(body)).hexdigest()


def build_manifest():
    files, dir_sets = collect()
    manifest = {
        "version": 1,
        "generatedAt": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "note": "评估面清单：构建器/测试/校验器。改动这些文件后必须重新签发。",
        "files": files,
        "dirSets": dir_sets,
    }
    manifest["selfSeal"] = compute_seal(manifest)
    return manifest


def git(*args):
    """跑一个只读 git 命令，返回 (ok, stdout)。git 不可用时 ok=False。"""
    try:
        p = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                           text=True, timeout=20)
        return p.returncode == 0, (p.stdout or "").strip()
    except Exception:
        return False, ""


def check_git_tracked():
    """清单必须已被 git 跟踪，且**与 HEAD 一致**（已提交、工作树干净）。

    返回 (ok, reason)。

    【为什么这条是"签发权归人"的着力点】重签会改变清单内容。只要清单是
    **被跟踪**的，这个改动就必然出现在 git 里 —— `git status` /
    `git diff --cached` / review / CI 都看得见，谁在什么时候放宽了判据留得下痕迹。
    agent 若想悄悄放宽判据，就必须连"提交"这一步也做掉（那本身是另一道
    更显眼的动作）。**不跟踪**就等于这条痕迹完全不存在 —— 这正是
    本轮修复前的状态（`?? data/eval-surface.json`）。

    【流程】签发 → `git add` → `git commit`。提交之后 status 变空，门开。
    签发之后**门仍然关着**，直到有人提交 —— 这正是"签发权归人"的机械含义
    （见 src/eval_surface.h 的同名注释）。
    """
    ok, tracked = git("ls-files", "--error-unmatch", "data/eval-surface.json")
    if not ok:
        return False, ("data/eval-surface.json 未被 git 跟踪 —— 重签将不留任何痕迹。"
                       "先执行：git add data/eval-surface.json")
    ok2, status = git("status", "--porcelain", "--", "data/eval-surface.json")
    if not ok2:
        return True, ""   # status 查不了（比如浅克隆）时不阻塞，只跳过这条
    # 【判据必须与 C++ 运行时门逐条一致】src/eval_surface.cpp 的 CheckGitTrust
    # 要求 `git status --porcelain -- data/eval-surface.json` **输出为空**
    # （已跟踪 + 已提交 + 工作树干净）。这里曾经放宽成"已暂存即可"，
    # 于是同一个不变量有了两个判定：清单只暂存没提交时，
    # tools/check_components.py 第 3 道放行（绿），而 dev.build/dev.test 拒绝启动。
    # 一边绿一边红，调用方只能猜 —— 而"两处判定同一件事"本身就是一条纪律。
    # 所以这里也要求空：已暂存（"M "）同样不合格。
    if status.strip():
        return False, ("data/eval-surface.json 与 HEAD 不一致（已跟踪但有未提交改动）—— "
                       "有人重签过但还没提交。先 git add 并提交它；"
                       "在此之前 dev.build/dev.test 会拒绝启动（fail-closed，这是对的）。")
    return True, ""


def diff(current, manifest):
    """返回差异描述列表；空列表表示一致。"""
    problems = []
    m_files = manifest.get("files", {})
    for rel, want in m_files.items():
        abs_path = os.path.join(REPO, rel)
        if not os.path.isfile(abs_path):
            problems.append(f"{rel}（已删除）")
            continue
        got = sha256(abs_path)
        if got != want:
            problems.append(f"{rel}（已修改）")

    m_dirs = manifest.get("dirSets", {})
    for rel_dir, entry in m_dirs.items():
        want = set(entry.get("files", []))
        ext = entry.get("ext", "")
        abs_dir = os.path.join(REPO, rel_dir)
        now = set()
        if os.path.isdir(abs_dir):
            now = {
                n for n in os.listdir(abs_dir)
                if n.lower().endswith(ext) and os.path.isfile(os.path.join(abs_dir, n))
            }
        for name in sorted(want - now):
            problems.append(f"{rel_dir}/{name}（已删除）")
        for name in sorted(now - want):
            problems.append(f"{rel_dir}/{name}（新增，需重新签发）")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="签发或校验评估面清单")
    ap.add_argument("--check", action="store_true", help="只校验，不写入；不一致时退出码 1")
    ap.add_argument("--human-confirm", action="store_true",
                    help="签发时必须带：表示'人已确认这次放宽判据'。"
                         "不给就拒绝签发（AI 通道无法自己加上这个旗标）")
    args = ap.parse_args()

    if args.check:
        if not os.path.isfile(MANIFEST):
            print(f"评估面清单缺失：{MANIFEST}\n  先跑：python tools/gen_eval_baseline.py --human-confirm")
            return 1
        with open(MANIFEST, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        # ---- 第一层：清单自身未被手改 ----
        seal = manifest.get("selfSeal", "")
        if not seal:
            print("清单缺少 selfSeal —— 它是旧版签发的，或被人删掉了这一项。")
            print("  确认后重新签发：python tools/gen_eval_baseline.py --human-confirm")
            return 1
        if compute_seal(manifest) != seal:
            print("清单正文与 selfSeal 不符 —— **清单被直接改过**（没有走重签）。")
            print("  这比\"评估面被改\"更可疑：改清单的人想让哪些检查看起来仍然合法？")
            print("  git diff data/eval-surface.json 可以看到改了什么。")
            return 1

        problems = diff(None, manifest)
        if problems:
            print(f"评估面清单已过期（{len(problems)} 项）：")
            for p in problems[:40]:
                print(f"  · {p}")
            if len(problems) > 40:
                print(f"  · …（还有 {len(problems) - 40} 项）")
            print("\n  确认改动是有意的后，重新签发："
                  "python tools/gen_eval_baseline.py --human-confirm")
            return 1

        # ---- 第二层：重签痕迹必须被 git 记住 ----
        ok, why = check_git_tracked()
        if not ok:
            print(f"评估面清单可信度不足：\n  {why}")
            return 1

        print(f"评估面清单一致（{MANIFEST}）")
        return 0

    # ---- 签发 ----
    if not args.human_confirm:
        print("拒绝签发：缺少 --human-confirm。")
        print("  签发 = 宣布\"从此这份判据是可信的\"。它必须由人做，")
        print("  不能由自动化流程顺手完成 —— 否则\"改软判据 + 重签\"就是一条无人值守的通道。")
        return 2

    manifest = build_manifest()
    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    n_files = len(manifest["files"])
    n_dirs = sum(len(v["files"]) for v in manifest["dirSets"].values())
    print(f"已签发评估面清单：{MANIFEST}")
    print(f"  单文件 {n_files} 个，目录内文件 {n_dirs} 个")
    print(f"  selfSeal = {manifest['selfSeal'][:16]}…")
    ok, why = check_git_tracked()
    if not ok:
        print(f"\n  注意：{why}")
        print("  在提交之前，dev.build/dev.test 会拒绝启动（fail-closed，这是对的）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
