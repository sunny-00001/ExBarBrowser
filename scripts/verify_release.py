#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ExBarBrowser 发布产物验证器
============================
把 release runbook 里的人工核验固化成 PASS/FAIL 门禁。输入是 package_release.py
产出的 zip（默认自动找 dist/ 下版本最大的那个），检查：

  1. zip 完整、条目可读；
  2. 必需文件齐全（exe / WebView2Loader.dll / aichat 服务与 UI / manifest）；
  3. manifest 里每个文件的 sha256 与包内实际字节一致；
  4. manifest 版本 / tag / 完整 commit 与仓库 VERSION、当前 git HEAD 对齐；
  5. 包内无真实 API key 或 GitHub token；
  6. aichat/service/*.js 的本地 require() 依赖闭合；
  7. exe 资源里带 VERSIONINFO，且版本号与 manifest 一致（Windows 上用 PowerShell，
     其他平台跳过并明确标注 SKIP）；
  8. 确定性问题：manifest 的 created_utc 必须等于 source_date_epoch 对应的 UTC
     时间，而不是打包当天的墙钟时间。

依赖只有 Python 标准库；版本号检查在 Windows 上额外调用 PowerShell（只读）。

用法：
  python scripts/verify_release.py
  python scripts/verify_release.py dist/ExBarBrowser-v0.3.1-win-x64.zip
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
OUT_DIR = ROOT / "dist"
REQUIRED = (
    "ExBarBrowser.exe",
    "WebView2Loader.dll",
    "aichat/service/server.js",
    "aichat/service/config.js",
    "aichat/ui/index.html",
    "aichat/ui/app.js",
    "RELEASE-MANIFEST.json",
    "START-HERE.txt",
    "data/README.md",
)


class Checker:
    def __init__(self):
        self.pass_n = 0
        self.fail_n = 0
        self.skip_n = 0

    def ok(self, name, detail=""):
        self.pass_n += 1
        print(f"  PASS  {name}" + (f" — {detail}" if detail else ""))

    def fail(self, name, detail=""):
        self.fail_n += 1
        print(f"  FAIL  {name}" + (f" — {detail}" if detail else ""))

    def skip(self, name, detail=""):
        self.skip_n += 1
        print(f"  SKIP  {name}" + (f" — {detail}" if detail else ""))

    def expect(self, cond, name, detail=""):
        (self.ok if cond else self.fail)(name, detail)
        return bool(cond)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def find_latest_zip() -> Path:
    zips = sorted(OUT_DIR.glob("ExBarBrowser-v*-win-x64.zip"))
    if not zips:
        raise RuntimeError(f"{OUT_DIR} 下没有发布 zip，先执行 python scripts/package_release.py")
    return zips[-1]


def git_commit() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                           capture_output=True, text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:                                       # noqa: BLE001
        return ""


def verify_exe_version(exe: Path, checker: Checker, want: str) -> None:
    """Windows 上读 VERSIONINFO；读不到就 SKIP，不误判为 PASS。"""
    if sys.platform != "win32":
        checker.skip("exe VERSIONINFO", "非 Windows 平台")
        return
    # 把路径直接嵌进脚本（用单引号包裹），不依赖 $args 绑定 ——
    # -Command 后面跟位置参数在 Windows PowerShell 5.1 上不可靠。
    ps = (
        "$ErrorActionPreference='Stop'; "
        f"$v=(Get-Item -LiteralPath '{exe.as_posix()}').VersionInfo; "
        "[Console]::OutputEncoding=[System.Text.Encoding]::ASCII; "
        "Write-Output ('FILEVERSION=' + $v.FileVersion)"
    )
    try:
        r = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps],
                           capture_output=True, timeout=30)
        out = r.stdout.decode("ascii", errors="replace")
        line = next((x for x in out.splitlines() if x.strip().startswith("FILEVERSION=")), "")
        got = line.split("=", 1)[1].strip() if line else ""
    except Exception as e:                                  # noqa: BLE001
        checker.skip("exe VERSIONINFO", f"PowerShell 调用失败：{e}")
        return
    if not got:
        checker.skip("exe VERSIONINFO", "PowerShell 未返回版本（可能被安全策略拦截）")
        return
    checker.expect(got == f"{want}.0" or got == want, "exe VERSIONINFO",
                   f"FileVersion={got}, 期望 {want}.0")


def verify_node_requires(service_dir: Path, checker: Checker) -> None:
    missing = []
    for p in sorted(service_dir.glob("*.js")):
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"""require\(["']\./([^"']+)["']\)""", text):
            rel = m.group(1)
            candidates = [service_dir / rel]
            if not rel.endswith(".js"):
                candidates.append(service_dir / (rel + ".js"))
            if not any(c.is_file() for c in candidates):
                missing.append(f"{p.name} -> ./{rel}")
    checker.expect(not missing, "Node 本地依赖闭合",
                   "全部解析" if not missing else "; ".join(missing[:8]))


def run(zip_path: Path) -> int:
    c = Checker()
    print(f"[验证] {zip_path}")
    if not zip_path.is_file():
        c.fail("zip 存在", str(zip_path))
        return 1

    # 0. 校验和文件（若存在）
    sha_sidecar = zip_path.with_name(zip_path.name + ".sha256")
    if sha_sidecar.is_file():
        want = sha_sidecar.read_text(encoding="utf-8").split()[0]
        got = sha256_bytes(zip_path.read_bytes())
        c.expect(got == want, "zip SHA256 与 .sha256 一致", f"{got[:16]}…")
    else:
        c.skip("zip SHA256 校验文件", "未找到 .sha256")

    with tempfile.TemporaryDirectory(prefix="ebb_verify_") as td:
        root = Path(td)
        try:
            with zipfile.ZipFile(zip_path) as z:
                bad = z.testzip()
                names = z.namelist()
                c.expect(bad is None, "zip 完整性", f"{len(names)} 个条目" if bad is None else bad)
                z.extractall(root)
        except zipfile.BadZipFile as e:
            c.fail("zip 可读", str(e))
            return 1

        missing = [n for n in REQUIRED if n not in names]
        c.expect(not missing, "必需文件齐全",
                 "全部存在" if not missing else "缺: " + ", ".join(missing))

        manifest_path = root / "RELEASE-MANIFEST.json"
        if not manifest_path.is_file():
            c.fail("manifest 存在")
            return 1 if c.fail_n else 0
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        # 1. 文件哈希与大小
        mismatches = []
        for rel, meta in sorted(manifest.get("files", {}).items()):
            p = root / rel
            if not p.is_file():
                mismatches.append(f"{rel} 缺失")
                continue
            data = p.read_bytes()
            if sha256_bytes(data) != meta.get("sha256") or len(data) != meta.get("size"):
                mismatches.append(f"{rel} 哈希/大小不符")
        extra = sorted(set(names) - set(manifest.get("files", {})) - {"RELEASE-MANIFEST.json"})
        c.expect(not mismatches, "包内文件哈希匹配",
                 f"{len(manifest.get('files', {}))} 个文件" if not mismatches else "; ".join(mismatches[:8]))
        c.expect(not extra, "无 manifest 未登记文件",
                 "无" if not extra else ", ".join(extra[:8]))

        # 2. 版本、tag、commit
        want_version = VERSION_FILE.read_text(encoding="utf-8").strip()
        c.expect(manifest.get("version") == want_version, "manifest 版本 = VERSION",
                 f"{manifest.get('version')} / {want_version}")
        c.expect(manifest.get("tag") == f"v{want_version}", "manifest tag 一致",
                 str(manifest.get("tag")))
        c.expect(want_version in zip_path.name, "zip 文件名含版本号")
        head = git_commit()
        got_commit = manifest.get("git_commit", "")
        c.expect(bool(re.fullmatch(r"[0-9a-f]{40}", got_commit)), "manifest commit 是完整 SHA",
                 got_commit[:12])
        if head:
            c.expect(got_commit == head, "manifest commit = 当前 HEAD",
                     f"{got_commit[:12]} / {head[:12]}")
        else:
            c.skip("manifest commit = 当前 HEAD", "无法读取 git HEAD")

        # 3. 确定性时间
        epoch = manifest.get("source_date_epoch")
        created = manifest.get("created_utc", "")
        expected_created = (
            datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            if isinstance(epoch, int) and epoch > 0 else "unknown"
        )
        c.expect(created == expected_created, "created_utc 来自 commit time（可复现）",
                 f"{created} / 期望 {expected_created}")

        # 3b. exe 的 PE COFF TimeDateStamp 必须等于 source_date_epoch
        pe = (root / "ExBarBrowser.exe").read_bytes()
        if len(pe) > 0x40 and pe[:2] == b"MZ":
            pe_off = int.from_bytes(pe[0x3C:0x40], "little")
            stamp = int.from_bytes(pe[pe_off + 4 + 4:pe_off + 4 + 8], "little")
            c.expect(stamp == epoch, "exe PE 时间戳已归一化",
                     f"{stamp} / epoch {epoch}")
        else:
            c.fail("exe 是有效 PE")

        # 4. 密钥扫描：apiKey 只查 JSON（JS 注释里的 "apiKey": "sk-…" 是文档示例），
        #    GitHub token 扫全部文本。
        secret_hits = []
        for p in sorted(root.rglob("*")):
            if not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except Exception:                               # noqa: BLE001
                continue
            if p.suffix.lower() == ".json":
                for m in re.finditer(r'"apiKey"\s*:\s*"([^"]*)"', text):
                    v = m.group(1).strip()
                    if v and not v.startswith("YOUR_"):
                        secret_hits.append(f"{p.name}:apiKey")
            if re.search(r"gh[pousr]_[A-Za-z0-9]{20,}", text):
                secret_hits.append(f"{p.name}:token")
        c.expect(not secret_hits, "无真实密钥 / token",
                 "干净" if not secret_hits else ", ".join(secret_hits[:8]))

        # 5. Node 依赖闭合
        verify_node_requires(root / "aichat" / "service", c)

        # 6. exe 版本资源
        verify_exe_version(root / "ExBarBrowser.exe", c, want_version)

    print(f"\n[结果] PASS {c.pass_n} / FAIL {c.fail_n} / SKIP {c.skip_n}")
    return 1 if c.fail_n else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="ExBarBrowser 发布产物验证器")
    ap.add_argument("zip", type=Path, nargs="?", help="待验证 zip（默认取 dist/ 下最新版本）")
    args = ap.parse_args()
    try:
        return run(args.zip.resolve() if args.zip else find_latest_zip())
    except Exception as e:                                  # noqa: BLE001 —— CLI 报错要可读
        print(f"[验证失败] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
