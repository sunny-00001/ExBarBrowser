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
    std::wstring error;          // ok=false 时的原因（空则说明是"值不匹配"）

    // 断言串本身非法（左值不是状态字段 / 语法错）—— 这是**实现缺陷**，
    // 与"值不匹配"要分开：前者该报给开发者，后者是功能的真实结果。
    bool malformed = false;
};

// 求值一条断言。
//
//   expr   形如 "sortMode=$arg:by"
//   args   命令入参（供 $arg: 引用）
//   state  StateToJson 产出的状态对象（供左值取值）
//
// 左值必须存在于 state 里。不存在即 malformed —— 因为那意味着断言引用了
// 一个"读不回来"的字段，断言永远不会真正生效（假绿），比没有断言更危险。
Result Check(const std::wstring& expr, const json::Value& args, const json::Value& state);

// 批量求值。全部通过才 ok；任一失败保留第一条失败的原因（但失败项都进 list）。
//
// 为什么保留全部而不是短路：一次运行看到所有不符的地方，比修一处跑一次快得多。
std::vector<Result> CheckAll(const std::vector<std::wstring>& exprs,
                             const json::Value& args, const json::Value& state);

// 把结果序列化成 JSON（进 CallResult / MCP 响应）。
json::Value ToJson(const std::vector<Result>& results);

// 从 "a=b" 拆出左右两半。返回 false 表示语法错（没有 '='）。
bool SplitExpr(const std::wstring& expr, std::wstring* lhs, std::wstring* rhs);

}  // namespace assert_check
