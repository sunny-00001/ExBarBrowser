#!/usr/bin/env bash
# 全量单测：编 + 跑 tests/ 下的每一个 .cpp（用下方 for 循环自动枚举，数量随仓库增长）。
# 数量不写死在这里 —— 曾写"当前 32 个"，实际涨到 39 个却长期没人改，口径就漂了。
#
# 【为什么要这个脚本】文档一直写着"tests/ 有 N 个单测"，但从来没人能一键跑 ——
# 每个测试直接链 src/*.cpp，需要的 `.cpp` 各不相同（`keymap_test` 要 str+log+platform_win，
# `popup_layout_test` 要 icon_glyph，`commands_test` 还要 cmd_naming …）。
# 于是实际跑的人只会跑文档里列的那几条，其余长期"存在但没人运行"
# （`keymap_test` 就是因此红着没人发现，见 CHANGELOG）。
#
# 【这里怎么解决】**复用一套专属的 obj，而不是为每个测试重新推依赖**：
#   1. 先跑一次构建，把 obj 刷新到最新；
#   2. 链接时取这批 obj，**排除** `main.obj`（它含 WinMain）、`*_test.obj`
#      与 `*.check.obj`（语法检查副本，与正式 obj 同符号 → 重复定义）；
#   3. 测试文件本身单独编，与这批 obj 一起链接。
#
# 【并行会话隔离】OBJDIR 指向 $OUT/obj、产物叫 ExBarBrowser.test.exe —— 与
# 主构建的 obj/ + ExBarBrowser.exe **物理分开**。多会话同时编译时，主构建清空
# 重建 obj/ 不会把测试链接到一半的 obj 抽走（曾导致 25 个测试同时 COMPILE-FAIL）。
# build_fast.py 已支持 --objdir / --out，这里只是用上它。
#
# 用法：
#   bash tests/run_all.sh            # 全量
#   bash tests/run_all.sh keymap     # 只跑名字含 keymap 的
#
# 退出码 = 失败条数（0 即全绿）。
#
# 【退出码 77 = "无法运行"】有测试需要交互桌面才能测（clipboard_test 要打开
# 剪贴板，沙箱/非交互会话下 GetLastError=5 ACCESS_DENIED）。这类条目**不是**
# 代码结论，混进失败里会让全量跑长期红着一条假问题，真正的回归反而被"这条
# 一直红"掩盖。所以：77 单列为"无法运行"，不计入退出码，但一定打印出来 ——
# 绝不静默算绿。
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
-lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luxtheme -ldwmapi -lcomctl32 -lgdi32 -luser32 -loleaut32 -lbcrypt \
-luiautomationcore"

OUT="${TMPDIR:-/tmp}/ebb_tests"
OBJDIR="$OUT/obj"                       # 测试专属中间目录，与主构建 obj/ 物理隔离
mkdir -p "$OUT"

# ---- 会话锁：同一时刻只允许一个会话跑这套测试 ----
#
# 【为什么需要】多个会话在同一个仓库里同时跑这套测试时会撞车，而且
# **两种撞车都伪装成"代码坏了"**：
#   ① 链接产物写到同一个 exe 路径 → "Permission denied"（文件正被另一个会话占用），
#      报错长得像编译器/权限问题，跟代码毫无关系；
#   ② 两个会话同时往同一个 obj 目录写 → 另一个会话可能读到**写了一半**的 .obj，
#      链接期报出莫名其妙的 undefined symbol。
# 假错比真错更费时间：它把排查方向带偏，而真问题还在那儿。
#
# 【锁的实现】mkdir 是原子的，当锁用。持有者进程没了 = 陈旧锁，接管它
# （否则一个会话崩了就把它后面的会话全堵死）。
# 锁覆盖**整段**（构建 + 链接 + 运行）：只锁构建那一段的话，
# 另一个会话仍可能在链接期间读到正在被改写的 obj。
#
# 【删除要用相对路径】仓库的删除安全钩子拒绝**带盘符前缀**的路径
# （"embedded drive prefix is not allowed"），而 $OUT 正是这种路径。
# 所以进到 $OUT 里用相对路径删，否则锁永远清不掉 —— 下个会话就得等陈旧锁。
LOCKDIR="$OUT/.lock"
lock_owner=""
lock_waited=0
while ! mkdir "$LOCKDIR" 2>/dev/null; do
    lock_owner=$(cat "$LOCKDIR/pid" 2>/dev/null || echo "")
    if [ -n "$lock_owner" ] && ! kill -0 "$lock_owner" 2>/dev/null; then
        echo "接管陈旧锁（原持有者 $lock_owner 已不在）"
        ( cd "$OUT" && rm -f ".lock/pid" && rmdir ".lock" ) 2>/dev/null
        continue
    fi
    if [ "$lock_waited" -gt 900 ]; then
        echo "等测试锁超过 15 分钟，放弃（持有者 $lock_owner 可能卡住了）"
        exit 2
    fi
    [ "$lock_waited" -eq 0 ] && echo "另一个会话正在跑测试（持有者 ${lock_owner:-未知}），等待…"
    sleep 3
    lock_waited=$((lock_waited + 3))
done
echo $$ > "$LOCKDIR/pid"
trap '( cd "$OUT" && rm -f ".lock/pid" && rmdir ".lock" ) 2>/dev/null' EXIT INT TERM

# 1. 刷新测试专属 obj（增量；已是最新时约 0.3s）
#    --objdir 隔离 obj/PCH/资源对象；--out 给独立 exe 名，不碰主构建的 ExBarBrowser.exe。
echo "刷新 obj（测试专属 $OBJDIR）…"
if ! bash build.sh --objdir "$OBJDIR" --out "$OUT/ExBarBrowser.test.exe" >"$OUT/_build.log" 2>&1; then
    echo "构建失败，先修构建再跑单测："
    grep -iE "error|warning" "$OUT/_build.log" | head -10
    exit 2
fi

# 2. 收集可复用的 obj（从测试专属目录，不读主构建的 obj/）
#
# 【为什么要剔除"孤儿 obj"】
# 这里用的是 `ls $OBJDIR/*.obj` —— 目录里有什么就链什么。而 obj 目录是
# **增量**的：源文件改了名（a.cpp → b.cpp）之后，a.obj 会留在原地。
# 主构建不中招（build_fast.py 按当前源清单链接，不扫目录），
# 但本脚本会 —— 于是同一个函数被链两次，链接期报 duplicate symbol，
# 看起来像"代码写坏了"，其实是缓存里躺着个孤儿。
# 真实踩过一次：app_jobs.cpp 改名成 app_commands_jobs.cpp 之后，
# 两个测试同时 COMPILE-FAIL（duplicate symbol: App::RegisterJobCommands）。
#
# 判据：obj 的名字必须能在 src/ 里找到同名的 .cpp。
# 唯一的例外是 app.res（资源对象，源是 .rc 不是 .cpp）。
OBJS=""
for o in "$OBJDIR"/*.obj; do
    [ -e "$o" ] || continue
    b=$(basename "$o")
    case "$b" in
        main.obj|*_test.obj) continue ;;
        *.check.obj) continue ;;
    esac
    base="${b%.obj}"
    if [ "$base" != "app.res" ] && [ ! -f "src/$base.cpp" ]; then
        # 孤儿：源文件改名/删除后留下的旧产物。跳过它，并说明一声 ——
        # 静默跳过会让"为什么少链了一个文件"变成猜谜。
        echo "  跳过孤儿 obj：$b（src/$base.cpp 已不存在）"
        continue
    fi
    OBJS="$OBJS $o"
done
OBJS="${OBJS# }"
if [ -z "$OBJS" ]; then
    echo "$OBJDIR 里没有可复用的目标文件（构建脚本的产物目录变了吗？）"
    exit 2
fi

filter="${1:-}"
pass=0; fail=0; cfail=0; norun=0
failed_names=""; norun_names=""

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
        if [ "$rc" -eq 77 ]; then
            # 前置条件不满足：既没验证也没否定，单独列出来。
            echo "NORUN(run=77) $name"
            head -3 "$OUT/$name.run.log" | sed 's/^/    /'
            norun=$((norun + 1)); norun_names="$norun_names $name"
            continue
        fi
        echo "FAIL(run=$rc) $name"
        tail -4 "$OUT/$name.run.log" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names="$failed_names $name"
    fi
done

echo
echo "通过 $pass / 无法运行 $norun / 运行失败 $fail / 编译失败 $cfail（共 $((pass + norun + fail + cfail)) 个测试）"
echo "日志：$OUT/"
[ -z "$norun_names" ] || echo "无法运行（环境不满足，未验证）：$norun_names"
[ -z "$failed_names" ] || echo "未通过：$failed_names"
[ $((fail + cfail)) -eq 0 ] || exit $((fail + cfail))
exit 0
