#!/usr/bin/env bash
# ExBarBrowser 主编译入口（Git Bash）
# 底层统一走 build_fast.py：PCH + 分 TU 增量 + 并行编译。
# 参数透传，例如：
#   bash build.sh --clean           # 全量重建
#   bash build.sh -j 16             # 16 路并行
#   bash build.sh --out /tmp/x.exe  # 输出到临时路径（不打断运行实例）
#
# 【缓存策略】不清缓存池。防混旧 obj 靠缓存键本身：build_fast.py 传给
# objcache 的 key 已包含"本 TU 全部头文件的内容指纹"（见 tools/objcache.py
# 的 cache_key/dep_fingerprint），头文件一变键就变，旧布局 obj 不可能命中。
# 这样跨会话加速照旧，日常编辑仍是增量编译。
# 万一怀疑缓存被污染，可手动执行 `python tools/objcache.py` 清一次池，
# 或加 --clean --no-objcache 全量重建；两者都不作为常规建包步骤。
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
