// 自断言求值实现。纯函数，不依赖窗口与命令总线（见头文件的理由）。
#include "assert_check.h"

#include <cmath>
#include <cstdio>

#include "str.h"

namespace assert_check {
namespace {

// 把 JSON 标量转成人可读文本，并保证**数值比较不受表示形式影响**。
//
// 为什么不用 snprintf("%g")：整数 2 会输出 "2"，而布尔与字符串要走不同分支；
// 更关键的是 2 与 2.0 必须相等 —— 那是同一个值，不该因为写入方式不同就判失败。
std::wstring ScalarText(const json::Value& v) {
    switch (v.type) {
    case json::Value::kBool:
        return v.b ? L"true" : L"false";
    case json::Value::kNumber: {
        // 整数值按整数输出，其余保留小数：避免 "2" 与 "2.000000" 这类噪声，
        // 也让 expect/actual 的对比结果对人可读。
        if (std::floor(v.num) == v.num && std::fabs(v.num) < 1e15) {
            wchar_t buf[32];
            std::swprintf(buf, 32, L"%.0f", v.num);
            return buf;
        }
        wchar_t buf[64];
        std::swprintf(buf, 64, L"%.6g", v.num);
        return buf;
    }
    case json::Value::kString:
        return v.str;
    case json::Value::kNull:
        return L"null";
    default:
        // 对象/数组不参与断言比较（断言只判标量）。给个类型名便于排查。
        return v.IsArray() ? L"<array>" : L"<object>";
    }
}

// 取入参里的值并转文本。支持 `$arg:key` 与 `$arg:key.length`。
//
// length 的用途：诸如 `entries.length=0`（清空后应当没有条目）这类断言，
// 比去比较整个数组现实得多。
bool ResolveArgRef(const std::wstring& ref, const json::Value& args,
                   std::wstring* out, std::wstring* err) {
    // ref 形如 "<key>" 或 "<key>.length"
    bool wantLength = false;
    std::wstring key = ref;
    const std::wstring suffix = L".length";
    if (key.size() > suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        wantLength = true;
        key.resize(key.size() - suffix.size());
    }
    if (key.empty()) {
        *err = L"$arg: 后没有键名";
        return false;
    }
    const json::Value* v = args.Find(key.c_str());
    if (!v) {
        *err = L"入参里没有 " + key;
        return false;
    }
    if (wantLength) {
        if (v->IsArray()) {
            *out = std::to_wstring(v->arr.size());
            return true;
        }
        if (v->IsString()) {
            *out = std::to_wstring(v->str.size());
            return true;
        }
        *err = key + L" 不是数组或字符串，没有 length";
        return false;
    }
    *out = ScalarText(*v);
    return true;
}

// 右侧求值：可能是 $arg:xxx，也可能是字面量。
bool ResolveRhs(const std::wstring& rhs, const json::Value& args,
                std::wstring* out, std::wstring* err) {
    if (rhs.rfind(L"$arg:", 0) == 0) {
        return ResolveArgRef(rhs.substr(5), args, out, err);
    }
    *out = rhs;
    return true;
}

}  // namespace

bool SplitExpr(const std::wstring& expr, std::wstring* lhs, std::wstring* rhs) {
    const size_t at = expr.find(L'=');
    if (at == std::wstring::npos) return false;
    *lhs = str::Trim(expr.substr(0, at));
    *rhs = str::Trim(expr.substr(at + 1));
    return !lhs->empty();
}

Result Check(const std::wstring& expr, const json::Value& args, const json::Value& state) {
    Result r;
    r.expr = expr;

    std::wstring lhs, rhs;
    if (!SplitExpr(expr, &lhs, &rhs)) {
        r.ok = false;
        r.malformed = true;
        r.error = L"断言格式应为 左值=右值（缺少 '=' 或左值为空）";
        return r;
    }

    // 左值必须真的能从状态里读到。读不到就是实现缺陷 ——
    // 那种断言永远为假或永远为真，等于假绿，比没有更危险。
    const json::Value* actual = state.Find(lhs.c_str());
    if (!actual) {
        r.ok = false;
        r.malformed = true;
        r.error = L"状态里没有字段 " + lhs + L"（断言引用了不可回读的字段）";
        return r;
    }

    std::wstring expectText;
    std::wstring err;
    if (!ResolveRhs(rhs, args, &expectText, &err)) {
        r.ok = false;
        r.malformed = true;
        r.error = err;
        return r;
    }

    r.expect = expectText;
    r.actual = ScalarText(*actual);

    // 字符串比较不区分大小写：路径/名字在 Windows 上本就如此，
    // 而断言写成 "$arg:path" 时不该因为盘符大小写判失败。
    const bool equal = actual->IsString()
                           ? str::IEquals(r.actual, r.expect)
                           : (r.actual == r.expect);
    r.ok = equal;
    return r;
}

std::vector<Result> CheckAll(const std::vector<std::wstring>& exprs,
                             const json::Value& args, const json::Value& state) {
    std::vector<Result> out;
    out.reserve(exprs.size());
    for (const auto& e : exprs) out.push_back(Check(e, args, state));
    return out;
}

json::Value ToJson(const std::vector<Result>& results) {
    json::Value arr = json::Value::MakeArray();
    for (const auto& r : results) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"ok", json::Value::MakeBool(r.ok));
        o.Set(L"expr", json::Value::MakeStr(r.expr));
        o.Set(L"expect", json::Value::MakeStr(r.expect));
        o.Set(L"actual", json::Value::MakeStr(r.actual));
        if (!r.error.empty()) o.Set(L"error", json::Value::MakeStr(r.error));
        if (r.malformed) o.Set(L"malformed", json::Value::MakeBool(true));
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

}  // namespace assert_check
