#!/usr/bin/env bash
# ExBarBrowser 主编译入口（Git Bash）
# 底层统一走 build_fast.py：PCH + 分 TU 增量 + 并行编译。
# 参数透传，例如：
#   bash build.sh --clean           # 全量重建
#   bash build.sh -j 16             # 16 路并行
#   bash build.sh --out /tmp/x.exe  # 输出到临时路径（不打断运行实例）
set -e
cd "$(dirname "$0")"

if command -v python >/dev/null 2>&1; then
  PY=python
elif command -v py >/dev/null 2>&1; then
  PY="py -3"
else
  echo "BUILD FAILED: 未找到 python（build_fast.py 需要 python3）"
  exit 1
fi

$PY build_fast.py "$@"
