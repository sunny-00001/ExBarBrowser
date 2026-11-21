#!/usr/bin/env bash
# 全量单测：编 + 跑 tests/ 下的每一个 .cpp（当前 32 个）。
#
# 【为什么要这个脚本】文档一直写着"tests/ 有 N 个单测"，但从来没人能一键跑 ——
# 每个测试直接链 src/*.cpp，需要的 `.cpp` 各不相同（`keymap_test` 要 str+log+platform_win，
# `popup_layout_test` 要 icon_glyph，`commands_test` 还要 cmd_naming …）。
# 于是实际跑的人只会跑文档里列的那几条，其余长期"存在但没人运行"
# （`keymap_test` 就是因此红着没人发现，见 CHANGELOG）。
#
# 【这里怎么解决】**复用主构建的 obj**，而不是为每个测试重新推依赖：
#   1. 先跑一次 `bash build.sh`（增量，通常 <1s）把 obj 刷新到最新；
#   2. 链接时取 `obj/*.obj`，**排除** `main.obj`（它含 WinMain）、`*_test.obj`
#      与 `*.check.obj`（构建脚本为语法检查生成的副本，与正式 obj 同符号 → 重复定义）；
#   3. 测试文件本身单独编，与这批 obj 一起链接。
#
# 这样每条测试只需 ~3 秒链接，且依赖关系由主构建保证 —— 不存在"手写依赖表过期"的问题。
#
# 用法：
#   bash tests/run_all.sh            # 全量
#   bash tests/run_all.sh keymap     # 只跑名字含 keymap 的
#
# 退出码 = 失败条数（0 即全绿）。
#
# 【R21】本脚本只编译并运行控制台程序，不向任何窗口发消息、不动光标、不抢焦点。

set -u

CC="${CC:-/c/Program Files/Windhawk/Compiler/bin/clang++.exe}"
[ -x "$CC" ] || CC="C:/Program Files/Windhawk/Compiler/bin/clang++.exe"
if [ ! -f "$CC" ]; then
    echo "找不到编译器：$CC"
    echo "用 CC=<路径> bash tests/run_all.sh 指定。"
    exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

FLAGS="-std=c++20 -O1 -target x86_64-w64-mingw32 -DUNICODE -D_UNICODE \
-DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 -Isrc"
LIBS="-static -static-libstdc++ -static-libgcc \
-lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luxtheme -ldwmapi -lcomctl32 -lgdi32 -luser32 -loleaut32 -lbcrypt"

OUT="${TMPDIR:-/tmp}/ebb_tests"
mkdir -p "$OUT"

# 1. 刷新 obj（增量；已是最新时约 0.3s）
echo "刷新 obj …"
if ! bash build.sh --out "$OUT/_warm.exe" >"$OUT/_build.log" 2>&1; then
    echo "构建失败，先修构建再跑单测："
    grep -iE "error|warning" "$OUT/_build.log" | head -10
    exit 2
fi

# 2. 收集可复用的 obj
OBJS=$(ls obj/*.obj 2>/dev/null \
       | grep -vE "obj/(main|.*_test)\.obj$" \
       | grep -v "\.check\.obj$" \
       | tr '\n' ' ')
if [ -z "$OBJS" ]; then
    echo "obj/ 里没有可复用的目标文件（构建脚本的产物目录变了吗？）"
    exit 2
fi

filter="${1:-}"
pass=0; fail=0; cfail=0
failed_names=""

for t in tests/*.cpp; do
    name=$(basename "$t" .cpp)
    if [ -n "$filter" ] && [[ "$name" != *"$filter"* ]]; then continue; fi
    exe="$OUT/$name.exe"

    # shellcheck disable=SC2086
    if ! "$CC" $FLAGS "$t" $OBJS $LIBS -o "$exe" >"$OUT/$name.build.log" 2>&1; then
        echo "COMPILE-FAIL $name"
        grep -oE "(undefined symbol: |duplicate symbol: )[A-Za-z_][A-Za-z0-9_:]*" \
            "$OUT/$name.build.log" | sort -u | head -4 | sed 's/^/    /'
        cfail=$((cfail + 1)); failed_names="$failed_names $name"
        continue
    fi

    # 工作目录：看源码里出现的**字符串字面量** L"../data/..." 还是 L"data/..."。
    #   判据取 `L"..."` 里的路径（不匹配注释）：commands_test 头注释里写着
    #   "不是 ../data/..."，按注释判会把 cwd 判反 —— 这坑实际踩过。
    #   keymap_test 把路径赋给变量再传，所以不能只认 `LoadKeymap(L"..."` 这种直调写法。
    if grep -qE 'L"\.\./data/' "$t"; then
        rundir="tests"
    else
        rundir="."
    fi

    if ( cd "$rundir" && "$exe" >"$OUT/$name.run.log" 2>&1 ); then
        echo "PASS $name"; pass=$((pass + 1))
    else
        rc=$?
        echo "FAIL(run=$rc) $name"
        tail -4 "$OUT/$name.run.log" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names="$failed_names $name"
    fi
done

echo
echo "通过 $pass / 运行失败 $fail / 编译失败 $cfail"
echo "日志：$OUT/"
[ -z "$failed_names" ] || echo "未通过：$failed_names"
[ $((fail + cfail)) -eq 0 ] || exit $((fail + cfail))
exit 0
