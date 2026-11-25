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

    // ================================================================
    // ④ 不等运算符 !=
    // ================================================================
    // 为什么单列一节：求值器支持 !=，而**静态检查一度不支持** ——
    // 检查里按第一个 '=' 切表达式，于是 "x!=0" 被切成左值 "x!"，
    // 报"左值不在 state 里"。结果就是：能写的断言被检查判成非法，
    // 于是这条能力事实上被封掉了。求值器与检查必须同时支持同一套语法。
    Expect(assert_check::Check(L"sortMode!=1", args, state).ok, "!= 成立时通过");
    Expect(!assert_check::Check(L"sortMode!=2", args, state).ok, "!= 不成立时失败");
    // 字符串同样走 !=（不区分大小写）
    Expect(assert_check::Check(L"activePath!=C:\\OTHER", args, state).ok,
           "!= 对字符串也成立");
    Expect(!assert_check::Check(L"activePath!=c:\\WORK", args, state).ok,
           "!= 对字符串按 Windows 语义比较（大小写不敏感）");

    // ================================================================
    // ⑤ $changed / $unchanged
    // ================================================================
    // 为什么需要：只有 $arg 时，能断言的只有"状态等于我传的参数"这一类命令。
    // 而"关掉一个标签之后标签数应该变少"、"后退之后路径应该变了"这类断言 ——
    // 也就是**最能暴露假动作**的那一类 —— 表达不出来：
    // 期望值不是任何一个入参，而是"和刚才不一样"。
    const json::Value before = StateOf(LR"({
        "sortMode": 2, "groupBy": 0, "viewMode": 4, "tabCount": 2,
        "bookmarkBarVisible": true, "activePath": "C:\\Work"
    })");
    Expect(assert_check::Check(L"tabCount=$changed", args, state, &before).ok,
           "$changed：值确实变了");
    // sortMode 在 before 与 after 里都是 2 —— 没变，所以 $changed 必须失败。
    // （这里刻意选"相等"的那一项：拿 viewMode（before=4 / after=5）做反例的话，
    //   它其实是**变了**的，断言本就该通过 —— 期望写反了会让人以为实现有问题。）
    Expect(!assert_check::Check(L"sortMode=$changed", args, state, &before).ok,
           "$changed：值没变时失败");
    Expect(assert_check::Check(L"sortMode=$unchanged", args, state, &before).ok,
           "$unchanged：值没变");
    Expect(!assert_check::Check(L"tabCount=$unchanged", args, state, &before).ok,
           "$unchanged：值变了时失败");
    // before 里必须也有这个字段，否则没法比 —— 判 malformed 而不是假绿。
    const json::Value other = StateOf(LR"({"sortMode": 1})");
    auto noBeforeField = assert_check::Check(L"tabCount=$changed", args, state, &other);
    Expect(!noBeforeField.ok && noBeforeField.malformed,
           "$changed：before 缺字段 → malformed");
    // 完全没有 before（调用方没采执行前状态）→ 判 malformed，
    // 绝不能当成"没变化所以通过"（那是最典型的假绿）。
    auto noBefore = assert_check::Check(L"tabCount=$changed", args, state, nullptr);
    Expect(!noBefore.ok && noBefore.malformed, "$changed：没有 before → malformed");
    // 结果里要带上执行前的值，调用方才知道"从几变到几"
    auto changed = assert_check::Check(L"tabCount=$changed", args, state, &before);
    Expect(changed.before == L"2" && changed.actual == L"3", "$changed 结果带 before/actual");

    // ================================================================
    // ⑥ $arg:xxx.trim
    // ================================================================
    // 为什么需要：入参会被产品逻辑规范化时，断言必须按**规范化后**的值比。
    // 两个真实例子：tab.rename 存的是去首尾空白后的名字；
    // search.find 搜的是去空白后的查询词。没有 trim，这两条明明可以断言的
    // 命令就只能退化成"写不出断言"。
    const json::Value trimArgs = ArgsOf(LR"({"name": "  项目 A  ", "query": "  txt  "})");
    const json::Value trimState = StateOf(LR"({
        "activeTabAlias": "项目 A", "searchQuery": "txt"
    })");
    Expect(assert_check::Check(L"activeTabAlias=$arg:name.trim", trimArgs, trimState).ok,
           "$arg:xxx.trim 去首尾空白后比较");
    Expect(!assert_check::Check(L"activeTabAlias=$arg:name", trimArgs, trimState).ok,
           "不写 .trim 时按原文比较（带空白）→ 不等");
    Expect(assert_check::Check(L"searchQuery=$arg:query.trim", trimArgs, trimState).ok,
           "$arg:xxx.trim 对另一键同样成立");
    // .trim 对非字符串入参不该崩：数字 trim 后仍是数字
    const json::Value numArgs = ArgsOf(LR"({"n": 2})");
    Expect(assert_check::Check(L"sortMode=$arg:n.trim", numArgs, state).ok,
           "$arg:xxx.trim 对数值入参不改变取值");

    // ================================================================
    // ⑦ 左值取长度
    // ================================================================
    const json::Value arrState = StateOf(LR"({"tabs": ["a","b","c"], "activeTabTitle": "abc"})");
    Expect(assert_check::Check(L"tabs.length=3", args, arrState).ok, "左值取数组长度");
    Expect(!assert_check::Check(L"tabs.length=2", args, arrState).ok, "数组长度不符时失败");
    Expect(assert_check::Check(L"activeTabTitle.length=3", args, arrState).ok,
           "左值取字符串长度");
    // 左值带 .length 但字段不是数组/字符串 → malformed
    auto badLen = assert_check::Check(L"sortMode.length=2", args, arrState);
    Expect(!badLen.ok && badLen.malformed, "非数组字段取 length → malformed");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
