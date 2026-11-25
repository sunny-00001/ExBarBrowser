// 自断言求值：把"执行成功了"与"效果对吗"分开。
//
// 【为什么单独一个模块而不塞进 ai_api】
//   求值是纯函数（输入：断言串 + 入参 + 状态快照；输出：通过与否 + 原因），
//   不依赖窗口、不依赖 CommandBus。独立出来才能单测 —— 而断言本身是判据，
//   判据不被测就等于没有判据。
//
// 【为什么断言不是回调】
//   与 Requirements 同一个理由：回调无法被枚举、无法被静态检查、无法生成文档与
//   测试骨架，最后必然退化成"每个命令自己写一段 if"。声明式字符串能被
//   check_ai_meta 校验（左值必须是可回读字段），也能被文档直接引用。
#pragma once
#include <string>
#include <vector>

#include "json.h"

namespace assert_check {

// 一条断言的结果。
//
// 刻意带 expect / actual 两个值：只说"失败"没用，调用方需要知道
// "期望什么、实际什么"才能决策（是参数错了？还是功能坏了？）。
struct Result {
    bool ok = true;              // 全部断言通过
    std::wstring expr;           // 原始断言串（回声，便于定位是多条里的哪一条）
    std::wstring expect;         // 期望值（人可读）
    std::wstring actual;         // 实际值（人可读）
    std::wstring before;         // 执行前的值（只有 $changed/$unchanged 断言才有）
    std::wstring error;          // ok=false 时的原因（空则说明是"值不匹配"）

    // 断言串本身非法（左值不是状态字段 / 语法错）—— 这是**实现缺陷**，
    // 与"值不匹配"要分开：前者该报给开发者，后者是功能的真实结果。
    bool malformed = false;
};

// 求值一条断言。
//
//   expr   形如 "sortMode=$arg:by" / "activePath!=C:\\" / "tabCount=$changed"
//   args   命令入参（供 $arg: 引用）
//   state  StateToJson 产出的状态对象（供左值取值）
//   before 执行**之前**的状态对象（供 $changed / $unchanged 比较；可为 nullptr，
//          此时用到这两种右值的断言判为 malformed —— 宁可报错也不要假绿）
//
// 【断言串的三种右值】
//   $arg:<key>[.length|.trim]  等于入参的取值（"我刚传的那个"）
//   $changed             与执行前**不同**（"状态确实动了"）
//   $unchanged           与执行前**相同**（"这一步不该动它"）
//
// 【为什么需要 .trim】
//   入参会被产品逻辑规范化时，断言必须按**规范化后**的值比：
//   tab.rename 存的是去空白后的名字，search.find 搜的是去空白后的词。
//   没有 trim，这两条明明可以断言的命令就只能退化成"写不出断言"。
//
// 【为什么需要 $changed / $unchanged】
//   只有 $arg 时，能断言的只有"状态等于我传的参数"这一类命令（排序、分组、
//   切标签）。而"关掉一个标签页之后标签数应该变少"、"后退之后路径应该变了"
//   这类断言 —— 也就是**最能暴露假动作**的那一类 —— 表达不出来：
//   期望值不是任何一个入参，而是"和刚才不一样"。
//   $unchanged 则相反，它守的是**不该变的东西**：排序不该改书签数、
//   移动书签不该改书签数、恢复会话不该改已保存的会话。
//
// 【运算符】
//   lhs=rhs   相等
//   lhs!=rhs  不等（路径这类"要确保没走错地方"的场景用得上）
Result Check(const std::wstring& expr, const json::Value& args, const json::Value& state,
             const json::Value* before = nullptr);

// 批量求值。全部通过才 ok；任一失败保留第一条失败的原因（但失败项都进 list）。
//
// 为什么保留全部而不是短路：一次运行看到所有不符的地方，比修一处跑一次快得多。
std::vector<Result> CheckAll(const std::vector<std::wstring>& exprs,
                             const json::Value& args, const json::Value& state,
                             const json::Value* before = nullptr);

// 把结果序列化成 JSON（进 CallResult / MCP 响应）。
json::Value ToJson(const std::vector<Result>& results);

// 从 "a=b" 或 "a!=b" 拆出左右两半。返回 false 表示语法错（没有 '=' 或左值为空）。
// negate 为 true 表示原文用的是 '!='。
bool SplitExpr(const std::wstring& expr, std::wstring* lhs, std::wstring* rhs,
               bool* negate = nullptr);

}  // namespace assert_check
