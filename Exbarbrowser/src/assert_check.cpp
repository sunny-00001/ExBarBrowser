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
//
// 数组与对象走紧凑 JSON 文本：这样 "tabs=$changed"（标签顺序变了没有）
// 与 "tabCount=$changed" 是同一套机制，不必为数组另开一条断言语法。
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
        return json::Stringify(v, 0);
    }
}

// 取入参里的值并转文本。支持 `$arg:key`、`$arg:key.length`、`$arg:key.trim`。
//
// length 的用途：诸如 `entries.length=0`（清空后应当没有条目）这类断言，
// 比去比较整个数组现实得多。
//
// trim 的用途：**入参会被产品逻辑规范化时，断言必须按规范化后的值比**。
// 两个真实例子：
//   · tab.rename 存进标签的是去首尾空白后的名字（"全是空格"当没改名），
//     于是 "activeTabAlias=$arg:name" 在名字带空白时必然不符；
//   · search.find 搜的是去空白后的查询词（"只打空格＝没打"）。
// 没有 trim 时，这两条命令就只能退化成"写不出断言"——
// 而它们其实完全可以断言，只是要比的不是原文。
bool ResolveArgRef(const std::wstring& ref, const json::Value& args,
                   std::wstring* out, std::wstring* err) {
    // ref 形如 "<key>" / "<key>.length" / "<key>.trim"
    bool wantLength = false;
    bool wantTrim = false;
    std::wstring key = ref;
    const std::wstring suffix = L".length";
    if (key.size() > suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        wantLength = true;
        key.resize(key.size() - suffix.size());
    } else {
        const std::wstring tsuffix = L".trim";
        if (key.size() > tsuffix.size() &&
            key.compare(key.size() - tsuffix.size(), tsuffix.size(), tsuffix) == 0) {
            wantTrim = true;
            key.resize(key.size() - tsuffix.size());
        }
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
    if (wantTrim) *out = str::Trim(*out);
    return true;
}

// 右值的三种形态。
enum class RhsKind {
    kValue,     // 字面量或 $arg:xxx —— 一个具体的值
    kChanged,   // $changed —— 与执行前不同
    kUnchanged, // $unchanged —— 与执行前相同
};

bool ResolveRhs(const std::wstring& rhs, const json::Value& args, RhsKind* kind,
                std::wstring* out, std::wstring* err) {
    if (rhs == L"$changed") {
        *kind = RhsKind::kChanged;
        *out = L"（与执行前不同）";
        return true;
    }
    if (rhs == L"$unchanged") {
        *kind = RhsKind::kUnchanged;
        *out = L"（与执行前相同）";
        return true;
    }
    *kind = RhsKind::kValue;
    if (rhs.rfind(L"$arg:", 0) == 0) {
        return ResolveArgRef(rhs.substr(5), args, out, err);
    }
    *out = rhs;
    return true;
}

// 取状态里的字段。支持 "<field>" 与 "<field>.length"（数组/字符串长度）。
//
// 返回**原始 JSON 值**（指向 state 内部，不拷贝）：比较时要按类型决定
// 是不是大小写不敏感（路径在 Windows 上本就如此），只留文本会把
// "true" 与 "TRUE"、1 与 "1" 这类差别抹平。
const json::Value* ResolveStateValue(const std::wstring& lhs, const json::Value& state,
                                     bool* wantLength, std::wstring* err) {
    *wantLength = false;
    std::wstring key = lhs;
    const std::wstring suffix = L".length";
    if (key.size() > suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        *wantLength = true;
        key.resize(key.size() - suffix.size());
    }
    const json::Value* v = state.Find(key.c_str());
    if (!v) {
        *err = L"状态里没有字段 " + key + L"（断言引用了不可回读的字段）";
        return nullptr;
    }
    if (*wantLength && !(v->IsArray() || v->IsString())) {
        *err = key + L" 不是数组或字符串，没有 length";
        return nullptr;
    }
    return v;
}

// 字段的值文本。length 请求时给长度，否则给标量文本。
std::wstring FieldText(const json::Value* v, bool wantLength) {
    if (wantLength) {
        return v->IsArray() ? std::to_wstring(v->arr.size())
                            : std::to_wstring(v->str.size());
    }
    return ScalarText(*v);
}

// 两个字段文本是否"相等"。字符串按 Windows 语义不区分大小写；
// 其余严格相等（数字 2 与 "2" 不该混为一谈）。
bool TextEqual(const json::Value* actual, bool wantLength, const std::wstring& expect) {
    const std::wstring a = FieldText(actual, wantLength);
    if (wantLength) return a == expect;
    if (actual->IsString()) return str::IEquals(a, expect);
    return a == expect;
}

}  // namespace

bool SplitExpr(const std::wstring& expr, std::wstring* lhs, std::wstring* rhs, bool* negate) {
    // 先找 '!=' 再找 '='：否则 "!=" 会被当成左值末尾带 '!' 的普通 '='。
    size_t at = expr.find(L"!=");
    bool neg = false;
    if (at == std::wstring::npos) {
        at = expr.find(L'=');
    } else {
        neg = true;
    }
    if (at == std::wstring::npos) return false;
    *lhs = str::Trim(expr.substr(0, at));
    *rhs = str::Trim(expr.substr(at + (neg ? 2 : 1)));
    if (negate) *negate = neg;
    return !lhs->empty();
}

Result Check(const std::wstring& expr, const json::Value& args, const json::Value& state,
             const json::Value* before) {
    Result r;
    r.expr = expr;

    std::wstring lhs, rhs;
    bool negate = false;
    if (!SplitExpr(expr, &lhs, &rhs, &negate)) {
        r.ok = false;
        r.malformed = true;
        r.error = L"断言格式应为 左值=右值 或 左值!=右值（缺少 '=' 或左值为空）";
        return r;
    }

    // 左值必须真的能从状态里读到。读不到就是实现缺陷 ——
    // 那种断言永远为假或永远为真，等于假绿，比没有更危险。
    bool wantLength = false;
    std::wstring err;
    const json::Value* actual = ResolveStateValue(lhs, state, &wantLength, &err);
    if (!actual) {
        r.ok = false;
        r.malformed = true;
        r.error = err;
        return r;
    }

    RhsKind kind = RhsKind::kValue;
    std::wstring expectText;
    if (!ResolveRhs(rhs, args, &kind, &expectText, &err)) {
        r.ok = false;
        r.malformed = true;
        r.error = err;
        return r;
    }

    r.expect = expectText;
    r.actual = FieldText(actual, wantLength);

    if (kind == RhsKind::kValue) {
        const bool equal = TextEqual(actual, wantLength, expectText);
        r.ok = negate ? !equal : equal;
        return r;
    }

    // ---- $changed / $unchanged：与执行前的状态比 ----
    if (!before) {
        r.ok = false;
        r.malformed = true;
        r.error = L"断言用了 " + rhs + L"，但没有提供执行前的状态（无法比较）";
        return r;
    }
    bool beforeLength = false;
    const json::Value* beforeValue =
        ResolveStateValue(lhs, *before, &beforeLength, &err);
    if (!beforeValue) {
        r.ok = false;
        r.malformed = true;
        r.error = L"执行前的状态里没有字段 " + lhs + L"（断言引用了不可回读的字段）";
        return r;
    }
    r.before = FieldText(beforeValue, beforeLength);
    const bool changed = r.actual != r.before;
    const bool predicateHolds = (kind == RhsKind::kChanged) ? changed : !changed;
    r.ok = negate ? !predicateHolds : predicateHolds;
    return r;
}

std::vector<Result> CheckAll(const std::vector<std::wstring>& exprs,
                             const json::Value& args, const json::Value& state,
                             const json::Value* before) {
    std::vector<Result> out;
    out.reserve(exprs.size());
    for (const std::wstring& e : exprs) out.push_back(Check(e, args, state, before));
    return out;
}

json::Value ToJson(const std::vector<Result>& results) {
    json::Value arr = json::Value::MakeArray();
    for (const Result& r : results) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"ok", json::Value::MakeBool(r.ok));
        o.Set(L"expr", json::Value::MakeStr(r.expr));
        o.Set(L"expect", json::Value::MakeStr(r.expect));
        o.Set(L"actual", json::Value::MakeStr(r.actual));
        if (!r.before.empty()) o.Set(L"before", json::Value::MakeStr(r.before));
        if (!r.error.empty()) o.Set(L"error", json::Value::MakeStr(r.error));
        if (r.malformed) o.Set(L"malformed", json::Value::MakeBool(true));
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

}  // namespace assert_check
