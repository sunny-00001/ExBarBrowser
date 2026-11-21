// 自断言求值单测。
//
// 【为什么这个测试比别的更重要】断言是"判据的判据"—— 它判的是"功能做对了没"。
// 如果求值器自己有 bug（比如把 2 和 "2" 判成不等，或把不存在的字段判成通过），
// 那上层的所有验证都会假绿，而且没人会怀疑。
//
// 覆盖三类：
//   ① 值匹配的判定（含数值/布尔/字符串、大小写）
//   ② $arg: 引用（含 .length）
//   ③ 非法断言（左值不可回读 / 语法错）必须被识别为 malformed，不能静默通过
#include <cstdio>
#include <string>

#include "assert_check.h"
#include "json.h"

static int g_pass = 0, g_fail = 0;

static void Expect(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

static json::Value StateOf(const wchar_t* json_text) {
    json::Value v;
    std::wstring err;
    json::Parse(json_text, v, &err);
    return v;
}

static json::Value ArgsOf(const wchar_t* json_text) {
    json::Value v;
    std::wstring err;
    json::Parse(json_text, v, &err);
    return v;
}

int main() {
    std::printf("自断言求值\n");

    const json::Value state = StateOf(LR"({
        "sortMode": 2, "groupBy": 0, "viewMode": 5, "tabCount": 3,
        "bookmarkBarVisible": true, "activePath": "C:\\Work"
    })");
    const json::Value args = ArgsOf(LR"({
        "by": 2, "preset": 5, "name": "abc", "targets": ["a","b"], "path": "c:\\work"
    })");

    // ---- ① 值匹配 ----
    Expect(assert_check::Check(L"sortMode=2", args, state).ok, "数值相等");
    Expect(assert_check::Check(L"bookmarkBarVisible=true", args, state).ok, "布尔相等");
    Expect(!assert_check::Check(L"bookmarkBarVisible=false", args, state).ok, "布尔不等要判失败");
    Expect(assert_check::Check(L"viewMode=5", args, state).ok, "另一字段也读得到");
    // 字符串比较不区分大小写：Windows 路径语义
    Expect(assert_check::Check(L"activePath=c:\\WORK", args, state).ok,
           "字符串比较不区分大小写");

    // ---- ② $arg: 引用 ----
    Expect(assert_check::Check(L"sortMode=$arg:by", args, state).ok, "$arg 引用数值");
    Expect(assert_check::Check(L"viewMode=$arg:preset", args, state).ok, "$arg 引用另一键");
    Expect(assert_check::Check(L"activePath=$arg:path", args, state).ok,
           "$arg 引用字符串（大小写不敏感）");
    // .length：数组长度断言
    Expect(assert_check::Check(L"sortMode=$arg:targets.length", args, state).ok,
           "$arg:xxx.length 取数组长度");
    // 字符串 "abc" 长度 3 —— 用来证明同一语法对字符串也成立。
    // （这里刻意写 3 而不是 sortMode：拿一个恰好等于另一字段的值做断言，
    //   会让这条测试在实现只支持数组时也意外通过。）
    Expect(assert_check::Check(L"tabCount=$arg:name.length", args, state).ok,
           "$arg:xxx.length 对字符串也成立");
    // 入参值本身与期望不符时，断言应当失败
    Expect(!assert_check::Check(L"sortMode=$arg:preset", args, state).ok,
           "$arg 引用到不同值时应失败");

    // ---- ③ 非法断言必须被识别，不能静默通过 ----
    auto bad1 = assert_check::Check(L"noSuchField=1", args, state);
    Expect(!bad1.ok && bad1.malformed, "左值不在状态里 → malformed");

    auto bad2 = assert_check::Check(L"sortMode", args, state);
    Expect(!bad2.ok && bad2.malformed, "缺少 = → malformed");

    auto bad3 = assert_check::Check(L"sortMode=$arg:noSuchKey", args, state);
    Expect(!bad3.ok && bad3.malformed, "引用了不存在的入参 → malformed");

    auto bad4 = assert_check::Check(L"sortMode=$arg:targets.nolength", args, state);
    Expect(!bad4.ok, "未知后缀 → 失败（不是静默通过）");

    // ---- 失败结果必须带 expect/actual，便于决策 ----
    auto r = assert_check::Check(L"sortMode=1", args, state);
    Expect(!r.ok && r.expect == L"1" && r.actual == L"2",
           "失败时给出 expect 与 actual");

    // ---- 分离：ok 表示值匹配，malformed 表示写法错，两者不混 ----
    Expect(!bad1.ok && bad1.malformed && !r.malformed,
           "值不匹配不是 malformed（两类原因要分清）");

    // ---- 批量：全部通过才 ok ----
    auto all = assert_check::CheckAll({L"sortMode=2", L"viewMode=5"}, args, state);
    Expect(all.size() == 2 && all[0].ok && all[1].ok, "批量全通过");
    auto mixed = assert_check::CheckAll({L"sortMode=2", L"viewMode=1"}, args, state);
    Expect(mixed.size() == 2 && mixed[0].ok && !mixed[1].ok, "批量保留每条结果");

    // ---- ToJson 形状 ----
    json::Value j = assert_check::ToJson({r});
    Expect(j.IsArray() && j.arr.size() == 1, "ToJson 输出数组");
    Expect(j.arr[0].Find(L"ok") && j.arr[0].Find(L"expr") &&
               j.arr[0].Find(L"expect") && j.arr[0].Find(L"actual"),
           "ToJson 含 ok/expr/expect/actual");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
