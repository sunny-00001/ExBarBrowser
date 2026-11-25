#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ExBarBrowser 发布打包器（可移植版）
====================================
把发布构建产物打成一个"解压即用"的 Windows x64 便携包。依赖只有 Python 标准库。

为什么要单独做这一步：
  主程序不是完全零依赖 —— AI 面板运行时需要 WebView2Loader.dll（与 exe 同目录）、
  aichat/ 下的 Node 服务与 Web UI；而 data/ 是首次启动自动生成的**用户私有配置**，
  state/、logs/、cache/、runtime/ 则分别是状态、日志、缓存与运行期文件。
  绝不能把开发机上的 settings.json / 书签 / API key 打进去。
  发布一个光秃秃的 exe 会把这些约束全部丢给用户，正是这次要修的问题。

用法：
  python scripts/package_release.py
  python scripts/package_release.py --exe path/to/ExBarBrowser.exe --out dist --keep-stage

产物（默认在 dist/）：
  ExBarBrowser-v<version>-win-x64/      便于人工检查的暂存目录
  ExBarBrowser-v<version>-win-x64.zip   正式发布资产
  ExBarBrowser-v<version>-win-x64.zip.sha256
  RELEASE-MANIFEST-v<version>.json      与包内 manifest 同内容，便于 CI/Release 单独上传
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
DEFAULT_EXE = ROOT / "ExBarBrowser.release.exe"
LOADER_DLL = ROOT / "WebView2Loader.dll"
AICHAT = ROOT / "aichat"
OUT_DIR = ROOT / "dist"

# 只复制 AI 面板真正会跑到的文件：测试、临时脚本一律不进包。
# 配置模板已取消 —— aichat.json / models.json / mcp.json 由服务首次启动生成。
SERVICE_EXTRA = ("exbar-bridge.ps1",)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def read_version(override: str | None) -> str:
    raw = (VERSION_FILE.read_text(encoding="utf-8").strip()
           if override is None else override.strip())
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?", raw):
        raise RuntimeError(f"版本号格式非法（应为 x.y.z）：{raw!r}")
    return raw


def _git_output(args: list[str], default: str = "") -> str:
    try:
        r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                           text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else default
    except Exception:                                       # noqa: BLE001 —— git 不是打包的硬依赖
        return default


def git_commit() -> str:
    """完整 40 位 commit SHA —— 短 SHA 有歧义，manifest 里不够用来溯源。"""
    return _git_output(["rev-parse", "HEAD"], "unknown")


def source_date_epoch() -> tuple[int, str]:
    """提交时间作为可复现构建的唯一时间戳：返回 (epoch, ISO8601 UTC)。

    墙钟时间会让同一份源码两次构建出不同 hash，别人无法核对"GitHub 二进制
    确实来自这个 commit"。commit time 是源码自带的、确定的值。
    """
    raw = _git_output(["show", "-s", "--format=%ct", "HEAD"])
    try:
        epoch = int(raw)
    except ValueError:
        return 0, "unknown"
    iso = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return epoch, iso


def toolchain_info() -> dict:
    """审计用工具链指纹：只记录、不参与 hash，便于别人复现同一构建环境。"""

    def version_or_unknown(exe_path: str) -> str:
        try:
            r = subprocess.run([exe_path, "--version"], capture_output=True,
                               text=True, timeout=10)
            line = next((x for x in (r.stdout + r.stderr).splitlines() if x.strip()), "")
            return line.strip()[:160] if line else "unknown"
        except Exception:                                   # noqa: BLE001
            return "unknown"

    return {
        "python": sys.version.split()[0],
        "clang": version_or_unknown(r"C:/Program Files/Windhawk/Compiler/bin/clang++.exe"),
        "windres": version_or_unknown(r"D:/MinGW/bin/windres.exe"),
        "release_flags": ["-O2", "-DNDEBUG", "-s", "-target x86_64-w64-mingw32"],
    }


def normalize_pe_timestamp(path: Path, epoch: int) -> None:
    """把 exe 的 PE COFF 头时间戳改成确定值（commit time）。

    链接器默认写入当前墙钟时间，导致同一份源码两次构建的 exe hash 不同。
    本程序未做 Authenticode 签名，改这个 4 字节不影响任何运行期行为。
    """
    if epoch <= 0:
        return
    data = bytearray(path.read_bytes())
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError(f"不是有效的 PE 文件：{path}")
    pe_off = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise RuntimeError(f"PE 签名错误：{path}")
    # COFF File Header: Machine(2) + NumberOfSections(2) + TimeDateStamp(4) ...
    # TimeDateStamp 在 COFF 起始 +4；+8 是 PointerToSymbolTable，别写错。
    coff = pe_off + 4
    data[coff + 4:coff + 8] = epoch.to_bytes(4, "little")
    path.write_bytes(data)


def copy_file(src: Path, dst: Path) -> None:
    if not src.is_file():
        raise RuntimeError(f"缺少发布依赖：{src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def stage_aichat(stage: Path) -> None:
    """AI 面板运行时：Node 服务 + Web UI（不携带任何示例配置）。"""
    service_src = AICHAT / "service"
    service_dst = stage / "aichat" / "service"
    if not service_src.is_dir():
        raise RuntimeError(f"缺少 aichat/service：{service_src}")
    for p in sorted(service_src.glob("*.js")):
        name = p.name
        # *_test.js 是判据不是运行时；_*.js 是手工试验脚本。
        if name.endswith("_test.js") or name.startswith("_"):
            continue
        copy_file(p, service_dst / name)
    for name in SERVICE_EXTRA:
        copy_file(service_src / name, service_dst / name)

    ui_src = AICHAT / "ui"
    if not ui_src.is_dir():
        raise RuntimeError(f"缺少 aichat/ui：{ui_src}")
    shutil.copytree(ui_src, stage / "aichat" / "ui", dirs_exist_ok=True)
    copy_file(AICHAT / "README.md", stage / "aichat" / "README.md")
    copy_file(AICHAT / "package.json", stage / "aichat" / "package.json")


def assert_no_secrets(stage: Path) -> None:
    """发布包里不能出现真实密钥或凭据 token。

    apiKey 只查 JSON —— 真实配置只在 data/*.json 里；JS 源码注释里的
    `"apiKey": "sk-…"` 是文档示例，不是密钥。token 则扫全部文本文件。
    """
    for p in sorted(stage.rglob("*")):
        if not p.is_file():
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except Exception:                                   # noqa: BLE001 —— 二进制跳过
            continue
        if p.suffix.lower() == ".json":
            for m in re.finditer(r'"apiKey"\s*:\s*"([^"]*)"', text):
                value = m.group(1).strip()
                if value and not value.startswith("YOUR_"):
                    raise RuntimeError(f"疑似真实 API key 被打进发布包：{p.relative_to(stage)}")
        if re.search(r"gh[pousr]_[A-Za-z0-9]{20,}", text):
            raise RuntimeError(f"疑似 GitHub token 被打进发布包：{p.relative_to(stage)}")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def build_manifest(stage: Path, version: str, epoch: int, created_utc: str) -> dict:
    files = {}
    for p in sorted(stage.rglob("*")):
        if p.is_file():
            rel = p.relative_to(stage).as_posix()
            files[rel] = {"sha256": sha256(p), "size": p.stat().st_size}
    return {
        "schema": 2,
        "product": "ExBarBrowser",
        "version": version,
        "tag": f"v{version}",
        "target": "win-x64",
        "channel": "stable",
        # 确定性字段：同一 commit 的任何一次打包都得到同一个值。
        "source_date_epoch": epoch,
        "created_utc": created_utc,
        "git_commit": git_commit(),
        "toolchain": toolchain_info(),
        "entry": "ExBarBrowser.exe",
        "runtime_requirements": {
            "os": "Windows 10 1809+ / Windows 11",
            "webview2": "WebView2 Runtime（Evergreen）；仅 AI 面板需要",
            "node": ">=18，PATH 或 EXBAR_NODE；仅 AI 面板需要",
        },
        "notes": [
            "data/ state/ logs/ cache/ runtime/ 首次启动自动生成，不携带开发者配置、书签或密钥。",
            "aichat.json / models.json / mcp.json 由 AI 服务首次启动写入 data/。",
            "WebView2Loader.dll 必须与 ExBarBrowser.exe 同目录。",
            "exe 的 PE 时间戳已归一化为 commit time，便于复核构件来源。",
        ],
        "files": files,
    }


def zip_stage(stage: Path, zip_path: Path) -> None:
    """固定时间戳 + 排序，让相同内容得到相同 zip（便于复核与增量分发）。"""
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(stage.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, p.read_bytes())


def main() -> int:
    ap = argparse.ArgumentParser(description="ExBarBrowser 可移植发布包打包器")
    ap.add_argument("--version", help="覆盖 VERSION 文件里的版本号（一般不需要）")
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="发布构建 exe 路径")
    ap.add_argument("--out", type=Path, default=OUT_DIR, help="输出目录（默认 dist/）")
    ap.add_argument("--keep-stage", action="store_true",
                    help="保留暂存目录（默认也保留；此参数仅用于显式表达意图）")
    args = ap.parse_args()

    version = read_version(args.version)
    epoch, created_utc = source_date_epoch()
    exe = args.exe.resolve()
    if not exe.is_file():
        raise RuntimeError(f"找不到发布 exe：{exe}\n先执行 bash build.sh --release")
    if not LOADER_DLL.is_file():
        raise RuntimeError(f"找不到 WebView2Loader.dll：{LOADER_DLL}")

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage = out / f"ExBarBrowser-v{version}-win-x64"
    if stage.exists():
        shutil.rmtree(stage)              # 只删本脚本生成的同名暂存目录
    stage.mkdir(parents=True)

    staged_exe = stage / "ExBarBrowser.exe"
    copy_file(exe, staged_exe)
    normalize_pe_timestamp(staged_exe, epoch)
    copy_file(LOADER_DLL, stage / "WebView2Loader.dll")
    stage_aichat(stage)

    write_text(stage / "data" / "README.md", """data/ — 配置目录（首次启动自动生成）

发布包里本目录只有一个说明文件。profile 根下按职责分层：
  data/       settings.json / bookmarks.html / keymap.json / mouse.json
              aichat.json（AI 行为）/ models.json（模型与密钥）/ mcp.json
  state/      会话、历史、标签、窗口状态
  logs/       审计与事件日志（audit.jsonl / events.jsonl）
  cache/      WebView2 用户数据
  runtime/    AI 通道发现文件（进程退出即删）

这里放的是**用户私有数据**，所以发布包不带开发机的真实内容。
可以用命令 `GoConfigPath` 直接打开配置目录。
""")

    write_text(stage / "START-HERE.txt", f"""ExBarBrowser v{version} — Windows x64 便携版

【运行】
  1. 整包解压到任意目录（建议路径不含中文和空格）。
  2. 双击 ExBarBrowser.exe。无需安装，也不写系统注册表。

【前置条件】
  - Windows 10 1809+ / Windows 11
  - WebView2 Runtime（Evergreen）：仅 AI 面板需要。WebView2Loader.dll 已随包提供，
    但运行时本身由系统安装；没有 Runtime 时主浏览器仍可用，只是 AI 面板打不开。
  - Node.js >= 18：仅 AI 面板需要。可在 PATH 中，或用环境变量 EXBAR_NODE 指向 node.exe。

【首次使用 AI 面板】
  1. 首次启动后 AI 服务会在 data/ 下自动生成 aichat.json（行为配置）、
     models.json（模型目录）与 mcp.json（空 server 清单）；在设置里填好自己的
     baseUrl / apiKey / 模型 id。
  2. 填好密钥的 data/models.json 是私密文件，不要连同日志一起发出去。
  不填也能启动主浏览器；AI 面板会因为没有模型配置而无法调用。

【目录说明】
  ExBarBrowser.exe      主程序（资源里已嵌入版本号 {version}）
  WebView2Loader.dll    WebView2 加载器，必须与 exe 同目录
  aichat/               AI 面板的 Node 服务与 Web UI（无第三方 npm 依赖）
  data/                 配置（首次启动生成，含模型与密钥）
  state/                会话/历史/标签/窗口状态（首次启动生成）
  logs/                 审计与事件日志（首次启动生成）
  cache/                WebView2 用户数据（可整体删除）
  runtime/              AI 通道发现文件（进程退出即删）
  profiles/             MULTI/显式 profile 的完整隔离根（首次启动生成）
  RELEASE-MANIFEST.json 文件清单与 SHA-256，便于校验完整性

WebView2Loader.dll 版权归 Microsoft Corporation，随包分发遵循 Microsoft WebView2 SDK 许可。
""")

    assert_no_secrets(stage)
    manifest = build_manifest(stage, version, epoch, created_utc)
    write_text(stage / "RELEASE-MANIFEST.json",
               json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")

    zip_path = out / f"ExBarBrowser-v{version}-win-x64.zip"
    zip_stage(stage, zip_path)
    sha_path = out / f"{zip_path.name}.sha256"
    write_text(sha_path, f"{sha256(zip_path)}  {zip_path.name}\n")
    manifest_out = out / f"RELEASE-MANIFEST-v{version}.json"
    shutil.copy2(stage / "RELEASE-MANIFEST.json", manifest_out)

    print(f"[打包] 版本 {version} | git {manifest['git_commit']} | epoch {epoch}")
    print(f"[打包] 暂存目录 {stage}（{len(manifest['files'])} 个文件）")
    print(f"[打包] 压缩包   {zip_path}（{zip_path.stat().st_size:,} bytes）")
    print(f"[打包] 校验文件 {sha_path}")
    print(f"[打包] 清单     {manifest_out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:                                  # noqa: BLE001 —— CLI 报错要可读
        print(f"[打包失败] {e}", file=sys.stderr)
        sys.exit(1)
