// 旧式单字符串参数 → 具名参数的适配层（从 commands.cpp 拆出）。
//
// 【为什么单独成文件】这是一块自成一体的东西：把 "ViewMode:3" 这种旧规格
// 翻译成 {by:3}。它与"命令表里有哪些命令"无关，是**参数形状**的翻译，
// 放在 commands.cpp 里只会让那个已经很满的文件更长。
//
// 【唯一真源】键名与类型都从命令自己的 argsSchema 推导，不另维护映射表 ——
// 以前 ai_api 里的 kArgKeys 就是缺这个推导而不得不加的补丁。
#include "commands.h"

#include "json.h"
#include "str.h"

namespace {

// argsSchema 存的是 UTF-8 的 const char*；转换唯一实现在 str.cpp。
// 空 schema 的业务语义（"{}" = 无参数对象）留在这里。
std::wstring AnsiToWide(const char* s) {
    std::wstring w = str::FromUtf8(s);
    return w.empty() ? std::wstring(L"{}") : w;
}

// 从 schema 里挑出"旧字符串参数该落到哪个键"。
// 规则：优先 required[0]，否则 properties 的第一个键。这是唯一合理的默认 ——
// 旧规格 "ViewMode:3" 里那个 3 只可能对应命令声明的那个必填档位。
bool PickSingleKey(const json::Value& schema, std::wstring* key, std::wstring* type) {
    const json::Value* req = schema.Find(L"required");
    const json::Value* props = schema.Find(L"properties");
    if (!props || !props->IsObject() || props->obj.empty()) return false;

    if (req && req->IsArray() && !req->arr.empty()) {
        const json::Value& first = req->arr[0];
        if (first.IsString()) {
            const json::Value* prop = props->Find(first.str.c_str());
            if (prop) {
                *key = first.str;
                *type = prop->GetStr(L"type");
                return true;
            }
        }
    }
    *key = props->obj[0].first;
    *type = props->obj[0].second.GetStr(L"type");
    return true;
}

}  // namespace

Args ArgsFromLegacyArg(const CommandDef& def, const std::wstring& legacyArg) {
    if (legacyArg.empty()) return Args::Empty();

    json::Value schema;
    std::wstring perr;
    if (!def.argsSchema || !json::Parse(std::wstring(AnsiToWide(def.argsSchema)), schema, &perr) ||
        !schema.IsObject())
        return Args::Empty();

    std::wstring key, type;
    if (!PickSingleKey(schema, &key, &type)) return Args::Empty();

    json::Value out = json::Value::MakeObject();
    // 按 schema 声明的类型决定存数字还是字符串。以前所有参数都是字符串，
    // handler 各自 _wtoi；现在类型由 schema 决定，handler 直接取 Int()。
    if (type == L"integer" || type == L"number") {
        out.Set(key, json::Value::MakeInt(_wtoi(legacyArg.c_str())));
    } else if (type == L"boolean") {
        out.Set(key, json::Value::MakeBool(legacyArg == L"1" || str::IEquals(legacyArg.c_str(), L"true")));
    } else {
        out.Set(key, json::Value::MakeStr(legacyArg));
    }
    return Args::FromJson(out);
}

bool HasRequiredArgs(const CommandDef& def) {
    // 与 ArgsFromLegacyArg 同一套解析口径（AnsiToWide 的"空 schema = {}"也照用），
    // 于是"面板要不要追问"与"键位能不能标"永远基于同一个答案。
    if (!def.argsSchema || !def.argsSchema[0]) return false;
    json::Value schema;
    std::wstring perr;
    if (!json::Parse(AnsiToWide(def.argsSchema), schema, &perr) || !schema.IsObject())
        return false;
    const json::Value* req = schema.Find(L"required");
    return req && req->IsArray() && !req->arr.empty();
}
