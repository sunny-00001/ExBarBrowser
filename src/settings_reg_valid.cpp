// settings_reg 的取值校验与写入构造器 —— 实现。
//
// 声明在 settings_reg_valid.h；键表本体（"有哪些设置项"）在 settings_reg.cpp。
// 两者是两种职责：这里是"一个值能不能进这个字段"的唯一实现。
#include "settings_reg_valid.h"

#include <cmath>   // std::floor / std::trunc：拒绝带小数的数字，不做静默截断

#include "str.h"   // str::IEquals（枚举值比较，大小写不敏感）

namespace settings_reg {


// ---------------------------------------------------------------- 类型校验
//
// 刻意不做隐式转换："把 true 当成 1 写进 int 字段"这类静默换算，正是
// "设置改了但没生效"最常见的来源。报错一次，比留一个神秘 bug 好。

bool AsBool(const json::Value& v, bool* out) {
    if (!v.IsBool()) return false;
    *out = v.b;
    return true;
}

bool AsInt(const json::Value& v, int* out) {
    if (!v.IsNumber()) return false;
    // json 用 double 存数字：超出 int 范围就报类型错，而不是静默截成垃圾值。
    if (v.num < -2147483648.0 || v.num > 2147483647.0) return false;
    // 带小数的数字必须拒绝：静默截断（1.5 → 1）会让 setter 先写入截断值，
    // 而 Set 的写后回读拿原文比必然不一致 —— 于是命令报失败、内存却已被污染。
    // "取整"意图应显式表达，不应藏在这里。
    if (std::trunc(v.num) != v.num) return false;
    *out = static_cast<int>(v.num);
    return true;
}

bool AsStr(const json::Value& v, std::wstring* out) {
    if (!v.IsString()) return false;
    *out = v.str;
    return true;
}

bool AsStrArray(const json::Value& v, std::vector<std::wstring>* out) {
    if (!v.IsArray()) return false;
    out->clear();
    out->reserve(v.arr.size());
    for (const auto& e : v.arr) {
        if (!e.IsString()) return false;
        out->push_back(e.str);
    }
    return true;
}

bool AsIntArray(const json::Value& v, std::vector<int>* out) {
    if (!v.IsArray()) return false;
    out->clear();
    out->reserve(v.arr.size());
    for (const auto& e : v.arr) {
        int n = 0;
        if (!AsInt(e, &n)) return false;
        out->push_back(n);
    }
    return true;
}

// ---------------------------------------------------------------- 写构造器
//
// 77 个可写项如果各写一个 lambda，"某处忘了查范围"就会成为常态，而那种错误的
// 表现是"越界值被静默接受"。这里把校验收成唯一实现：类型不符 / 超范围都在
// 同一处报同一种话。

using Setter = std::function<bool(const json::Value&, std::wstring*)>;

Setter BoolSet(bool* field, const wchar_t* key) {
    return [field, key](const json::Value& v, std::wstring* err) {
        bool b = false;
        if (!AsBool(v, &b)) {
            if (err) *err = std::wstring(key) + L" 需要布尔值（true / false）";
            return false;
        }
        *field = b;
        return true;
    };
}

// lo == hi == 0 表示不查范围（用于没有明确边界、但必须拒绝非整数的项）。
Setter IntSet(int* field, const wchar_t* key, int lo, int hi) {
    return [field, key, lo, hi](const json::Value& v, std::wstring* err) {
        int n = 0;
        if (!AsInt(v, &n)) {
            if (err) *err = std::wstring(key) + L" 需要整数";
            return false;
        }
        if (hi > lo && (n < lo || n > hi)) {
            if (err)
                *err = std::wstring(key) + L" 超出范围 [" + std::to_wstring(lo) + L", " +
                       std::to_wstring(hi) + L"]，收到 " + std::to_wstring(n);
            return false;
        }
        *field = n;
        return true;
    };
}

// 范围 + 业务判据。两处拒绝的理由必须能区分：超范围说"范围"，业务不允许说
// rejectWhy —— 混成一句话，调用方就不知道是改值还是换档。
Setter IntCheckedSet(int* field, const wchar_t* key, int lo, int hi, AllowedFn allowed,
                     std::wstring rejectWhy) {
    return [field, key, lo, hi, allowed = std::move(allowed),
            rejectWhy = std::move(rejectWhy)](const json::Value& v, std::wstring* err) {
        int n = 0;
        if (!AsInt(v, &n)) {
            if (err) *err = std::wstring(key) + L" 需要整数";
            return false;
        }
        if (hi > lo && (n < lo || n > hi)) {
            if (err)
                *err = std::wstring(key) + L" 超出范围 [" + std::to_wstring(lo) + L", " +
                       std::to_wstring(hi) + L"]，收到 " + std::to_wstring(n);
            return false;
        }
        if (allowed && !allowed(n)) {
            if (err) *err = std::wstring(key) + L" " + rejectWhy;
            return false;
        }
        *field = n;
        return true;
    };
}

Setter StrSet(std::wstring* field, const wchar_t* key) {
    return [field, key](const json::Value& v, std::wstring* err) {
        std::wstring s;
        if (!AsStr(v, &s)) {
            if (err) *err = std::wstring(key) + L" 需要字符串";
            return false;
        }
        *field = s;
        return true;
    };
}

// 枚举型字符串：只能取登记过的值。静默接受会让配置进入"文档没定义"的状态，
// 而那种状态下的行为没有任何人验证过。
Setter StrEnumSet(std::wstring* field, const wchar_t* key, std::vector<std::wstring> allowed) {
    return [field, key, allowed](const json::Value& v, std::wstring* err) {
        std::wstring s;
        if (!AsStr(v, &s)) {
            if (err) *err = std::wstring(key) + L" 需要字符串";
            return false;
        }
        for (const auto& a : allowed)
            if (str::IEquals(s.c_str(), a.c_str())) {
                *field = s;
                return true;
            }
        std::wstring list;
        for (size_t i = 0; i < allowed.size(); ++i) {
            if (i) list += L" / ";
            list += allowed[i];
        }
        if (err) *err = std::wstring(key) + L" 只能取 " + list + L"，收到 " + s;
        return false;
    };
}

Setter StrSpec::SetterFor(std::wstring* field) const {
    // 拷出值语义的副本：Entry 里存的是 std::function，不能捕获 this。
    const std::wstring key = this->key;
    const std::vector<std::wstring> allowed = this->allowed;
    const std::function<std::wstring(std::wstring)> norm = this->norm;
    return [field, key, allowed, norm](const json::Value& v, std::wstring* err) {
        std::wstring s;
        if (!AsStr(v, &s)) {
            if (err) *err = key + L" 需要字符串";
            return false;
        }
        s = norm ? norm(s) : s;
        if (!allowed.empty()) {
            bool ok = false;
            for (const auto& a : allowed)
                if (str::IEquals(s.c_str(), a.c_str())) ok = true;
            if (!ok) {
                std::wstring list;
                for (size_t i = 0; i < allowed.size(); ++i) {
                    if (i) list += L" / ";
                    list += allowed[i];
                }
                if (err) *err = key + L" 只能取 " + list + L"，收到 " + s;
                return false;
            }
        }
        *field = s;   // 存的是**规范值**，不是请求原文
        return true;
    };
}

std::function<json::Value(const json::Value&)> StrSpec::CanonFor() const {
    const std::function<std::wstring(std::wstring)> norm = this->norm;
    return [norm](const json::Value& v) {
        return json::Value::MakeStr(norm && v.IsString() ? norm(v.str) : v.str);
    };
}

Setter IntArraySpec::SetterFor(std::vector<int>* field) const {
    // 值语义副本：Entry 里存的是 std::function，不能捕获 this。
    const std::wstring key = this->key;
    const int lo = this->lo, hi = this->hi;
    const std::function<std::vector<int>(std::vector<int>)> norm = this->norm;
    return [field, key, lo, hi, norm](const json::Value& v, std::wstring* err) {
        std::vector<int> a;
        if (!AsIntArray(v, &a)) {
            if (err) *err = key + L" 需要整数数组";
            return false;
        }
        for (int n : a) {
            if (n < lo || n > hi) {
                if (err)
                    *err = key + L" 的元素须在 [" + std::to_wstring(lo) + L", " +
                           std::to_wstring(hi) + L"]，收到 " + std::to_wstring(n);
                return false;
            }
        }
        *field = norm ? norm(std::move(a)) : std::move(a);   // 存的是规范形
        return true;
    };
}

std::function<json::Value(const json::Value&)> IntArraySpec::CanonFor() const {
    const std::function<std::vector<int>(std::vector<int>)> norm = this->norm;
    return [norm](const json::Value& v) {
        std::vector<int> a;
        if (norm && AsIntArray(v, &a)) return IntArrayValue(norm(std::move(a)));
        return v;
    };
}

Setter StrArraySet(std::vector<std::wstring>* field, const wchar_t* key) {
    return [field, key](const json::Value& v, std::wstring* err) {
        std::vector<std::wstring> a;
        if (!AsStrArray(v, &a)) {
            if (err) *err = std::wstring(key) + L" 需要字符串数组";
            return false;
        }
        *field = std::move(a);
        return true;
    };
}

// 整数数组，逐元素查范围（statusSegs 这类"段号集合"必须落在合法段号内）。
Setter IntArraySet(std::vector<int>* field, const wchar_t* key, int lo, int hi) {
    return [field, key, lo, hi](const json::Value& v, std::wstring* err) {
        std::vector<int> a;
        if (!AsIntArray(v, &a)) {
            if (err) *err = std::wstring(key) + L" 需要整数数组";
            return false;
        }
        for (int n : a) {
            // 闭区间 [lo, hi]：与 IntSet 同一套语义。半开区间在这里曾经是
            // "看似更严谨"的写法，但它与同一文件里的 IntSet 相反，调用方按
            // 直觉传 Seg_Count 会多放行一个越界段号。
            if (n < lo || n > hi) {
                if (err)
                    *err = std::wstring(key) + L" 的元素须在 [" + std::to_wstring(lo) + L", " +
                           std::to_wstring(hi) + L"]，收到 " + std::to_wstring(n);
                return false;
            }
        }
        *field = std::move(a);
        return true;
    };
}

// ---------------------------------------------------------------- 读构造器

json::Value StrArrayValue(const std::vector<std::wstring>& v) {
    json::Value a = json::Value::MakeArray();
    for (const auto& s : v) a.arr.push_back(json::Value::MakeStr(s));
    return a;
}

json::Value IntArrayValue(const std::vector<int>& v) {
    json::Value a = json::Value::MakeArray();
    for (int n : v) a.arr.push_back(json::Value::MakeInt(n));
    return a;
}

// tabMeta：路径 → 别名/颜色。只读导出成数组，每项 {path, alias, color}。
json::Value TabMetaValue(const std::vector<TabMeta>& v) {
    json::Value a = json::Value::MakeArray();
    for (const auto& t : v) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"path", json::Value::MakeStr(t.path));
        o.Set(L"alias", json::Value::MakeStr(t.alias));
        o.Set(L"color", json::Value::MakeInt(t.color));
        a.arr.push_back(std::move(o));
    }
    return a;
}

// hotkeyOverrides：setting → 按键。只读导出成对象（键是设置名，值是按键串）。
json::Value HotkeyValue(const std::vector<std::pair<std::wstring, std::wstring>>& v) {
    json::Value o = json::Value::MakeObject();
    for (const auto& kv : v) o.Set(kv.first, json::Value::MakeStr(kv.second));
    return o;
}

json::Value TabHistoryValue(const std::vector<TabHistory>& v) {
    json::Value a = json::Value::MakeArray();
    for (const auto& h : v) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"paths", StrArrayValue(h.paths));
        o.Set(L"idx", json::Value::MakeInt(h.idx));
        a.arr.push_back(std::move(o));
    }
    return a;
}
}  // namespace settings_reg
