// 数组参数校验的针对性验证（审查中发现的漏项）。
//
// 背景：ValidateAgainstSchema 早先只校验 integer/number/string/boolean，
// 遇到 type:"array" 直接跳过。后果是 FsDelete 传 {"targets":"不是数组"} 会
// 一路走到 handler，StrArray 返回空 -> 命令返回 false -> 报"命令执行失败"。
// 那个错误信息完全指不到"你传的不是数组"这个真正原因。
#include "common.h"

#include <cstdio>

#include "ai_api.h"
#include "json.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static ai::CallResult CallJson(const wchar_t* json) {
    json::Value v;
    std::wstring e;
    json::Parse(json, v, &e);
    return ai::Call(L"fs.delete", v, true);
}

// 直接调校验器，绕开 Call 的权限闸（FsDelete 是 destructive，
// 默认被 aiAllowDanger 挡住 —— 那是对的，但会掩盖我们想测的参数校验）。
static bool Validate(const wchar_t* argsJson, std::wstring* err) {
    json::Value args;
    std::wstring e;
    json::Parse(argsJson, args, &e);
    json::Value schema;
    json::Parse(std::wstring(
        L"{\"type\":\"object\",\"properties\":{\"targets\":{\"type\":\"array\","
        L"\"items\":{\"type\":\"string\"}}},\"required\":[\"targets\"],"
        L"\"additionalProperties\":false}"), schema, &e);
    return ai::ValidateAgainstSchema(schema, args, err);
}

int main() {
    std::printf("数组参数类型校验\n");

    std::wstring err;
    std::wstring e1;
    Check(!Validate(L"{\"targets\":\"notarray\"}", &e1), "非数组 -> 拒绝");
    // 关键：错误信息必须指向"数组"，而不是笼统的"执行失败"
    Check(e1.find(L"数组") != std::wstring::npos, "错误信息指明需要数组");
    std::printf("        error=%ls\n", e1.c_str());

    std::wstring e2;
    Check(!Validate(L"{\"targets\":[1,2]}", &e2), "元素非字符串 -> 拒绝");
    Check(e2.find(L"数组元素") != std::wstring::npos, "错误信息指明元素类型");
    std::printf("        error=%ls\n", e2.c_str());

    Check(Validate(L"{\"targets\":[]}", &err), "空数组合法");
    Check(Validate(L"{\"targets\":[\"C:\\\\x.txt\"]}", &err), "合法数组通过");

    std::printf("\n缺必填 / 多余字段\n");
    Check(!Validate(L"{}", &err), "缺 targets -> 拒绝");
    Check(err.find(L"缺少必填") != std::wstring::npos, "错误信息指明缺必填项");
    Check(!Validate(L"{\"targets\":[],\"extra\":1}", &err), "未声明字段 -> 拒绝");

    std::printf("\n权限闸本身仍然生效（不是被测试绕过）\n");
    ai::CallResult r = CallJson(L"{\"targets\":[\"C:\\\\x.txt\"]}");
    Check(!r.ok, "默认设置下 FsDelete 被拒（destructive）");
    Check(r.error == L"danger_disabled", "错误是 danger_disabled");

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
