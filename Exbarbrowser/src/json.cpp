#include "json.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {

namespace {

// double → int，**避免未定义行为**。
//
// 原来直接 `static_cast<int>(v->num)`：当 num 超出 int 范围（配置被手改成
// `"w": 10000000000`，或其它工具写入异常值），这在 C++ 里是 UB ——
// 实测 GCC/MSVC 都给出 INT_MIN，然后 INT_MIN 流进窗口尺寸计算，
// 再做 `work.right - w` 就是又一次有符号溢出，最终 CreateWindowEx 收到负宽高。
//
// NaN / inf 同样处理（JSON 语法不允许，但 `1e999` 会解析成 inf）。
// 越界时返回调用方给的默认值，而不是钳到 INT_MAX —— 钳到极值仍会让窗口
// 变得不可见，而"当作没写这个键"才是用户能预期的行为。
int ClampToInt(double d, int def) {
    if (!std::isfinite(d)) return def;
    if (d < static_cast<double>(INT_MIN) || d > static_cast<double>(INT_MAX)) return def;
    return static_cast<int>(d);
}

}  // namespace

const Value* Value::Find(const wchar_t* key) const {
    if (type != kObject || !key) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::wstring Value::GetStr(const wchar_t* key, const std::wstring& def) const {
    const Value* v = Find(key);
    return (v && v->type == kString) ? v->str : def;
}

bool Value::GetBool(const wchar_t* key, bool def) const {
    const Value* v = Find(key);
    if (!v) return def;
    if (v->type == kBool) return v->b;
    if (v->type == kNumber) return v->num != 0.0;
    return def;
}

int Value::GetInt(const wchar_t* key, int def) const {
    const Value* v = Find(key);
    if (!v || v->type != kNumber) return def;
    return ClampToInt(v->num, def);
}

void Value::Set(const std::wstring& key, Value v) {
    type = kObject;
    for (auto& kv : obj) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    obj.emplace_back(key, std::move(v));
}

Value Value::MakeStr(const std::wstring& s) {
    Value v;
    v.type = kString;
    v.str = s;
    return v;
}

Value Value::MakeBool(bool b) {
    Value v;
    v.type = kBool;
    v.b = b;
    return v;
}

Value Value::MakeInt(int n) {
    Value v;
    v.type = kNumber;
    v.num = n;
    return v;
}

Value Value::MakeNum(double n) {
    Value v;
    v.type = kNumber;
    v.num = n;
    return v;
}

Value Value::MakeArray() {
    Value v;
    v.type = kArray;
    return v;
}

Value Value::MakeObject() {
    Value v;
    v.type = kObject;
    return v;
}

namespace {

struct Parser {
    const std::wstring& s;
    size_t i = 0;
    int depth = 0;
    std::wstring err;

    explicit Parser(const std::wstring& text) : s(text) {}

    void Fail(const wchar_t* msg) {
        if (err.empty()) {
            // 行号：只为报错可读，O(n) 只走一次
            int line = 1;
            for (size_t k = 0; k < i && k < s.size(); ++k)
                if (s[k] == L'\n') ++line;
            wchar_t buf[128];
            swprintf(buf, 128, L"第 %d 行：%ls", line, msg);
            err = buf;
        }
    }

    void Ws() {
        while (i < s.size()) {
            wchar_t c = s[i];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                ++i;
            } else if (c == L'/' && i + 1 < s.size() && s[i + 1] == L'/') {
                while (i < s.size() && s[i] != L'\n') ++i;
            } else {
                break;
            }
        }
    }

    bool Lit(const wchar_t* word) {
        size_t n = wcslen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }

    bool String(std::wstring& out) {
        if (i >= s.size() || s[i] != L'"') {
            Fail(L"期望字符串");
            return false;
        }
        ++i;
        out.clear();
        while (i < s.size()) {
            wchar_t c = s[i++];
            if (c == L'"') return true;
            if (c != L'\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) break;
            wchar_t e = s[i++];
            switch (e) {
            case L'"': out.push_back(L'"'); break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/': out.push_back(L'/'); break;
            case L'b': out.push_back(L'\b'); break;
            case L'f': out.push_back(L'\f'); break;
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'u': {
                unsigned code = 0;
                for (int k = 0; k < 4; ++k) {
                    if (i >= s.size()) {
                        Fail(L"\\u 转义不完整");
                        return false;
                    }
                    wchar_t h = s[i++];
                    code <<= 4;
                    if (h >= L'0' && h <= L'9') code |= static_cast<unsigned>(h - L'0');
                    else if (h >= L'a' && h <= L'f') code |= static_cast<unsigned>(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') code |= static_cast<unsigned>(h - L'A' + 10);
                    else {
                        Fail(L"\\u 转义含非法字符");
                        return false;
                    }
                }
                out.push_back(static_cast<wchar_t>(code));
                break;
            }
            default:
                Fail(L"未知转义");
                return false;
            }
        }
        Fail(L"字符串未闭合");
        return false;
    }

    bool Number(Value& out) {
        size_t start = i;
        if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) ++i;
        bool digits = false;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
            ++i;
            digits = true;
        }
        if (i < s.size() && s[i] == L'.') {
            ++i;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
                ++i;
                digits = true;
            }
        }
        if (digits && i < s.size() && (s[i] == L'e' || s[i] == L'E')) {
            ++i;
            if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) ++i;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i;
        }
        if (!digits) {
            Fail(L"非法数字");
            return false;
        }
        out.type = Value::kNumber;
        out.num = wcstod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    bool ValueAt(Value& out) {
        if (++depth > 64) {
            Fail(L"嵌套过深");
            return false;
        }
        Ws();
        bool ok = false;
        if (i >= s.size()) {
            Fail(L"意外的结尾");
            return false;
        }
        wchar_t c = s[i];
        if (c == L'{') {
            ++i;
            out.type = Value::kObject;
            Ws();
            if (i < s.size() && s[i] == L'}') {
                ++i;
                --depth;
                return true;
            }
            while (true) {
                Ws();
                std::wstring key;
                if (!String(key)) break;
                Ws();
                if (i >= s.size() || s[i] != L':') {
                    Fail(L"对象成员缺少冒号");
                    break;
                }
                ++i;
                Value v;
                if (!ValueAt(v)) break;
                out.obj.emplace_back(key, std::move(v));
                Ws();
                if (i < s.size() && s[i] == L',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == L'}') {
                    ++i;
                    ok = true;
                    break;
                }
                Fail(L"对象缺少 }");
                break;
            }
        } else if (c == L'[') {
            ++i;
            out.type = Value::kArray;
            Ws();
            if (i < s.size() && s[i] == L']') {
                ++i;
                --depth;
                return true;
            }
            while (true) {
                Value v;
                if (!ValueAt(v)) break;
                out.arr.push_back(std::move(v));
                Ws();
                if (i < s.size() && s[i] == L',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == L']') {
                    ++i;
                    ok = true;
                    break;
                }
                Fail(L"数组缺少 ]");
                break;
            }
        } else if (c == L'"') {
            out.type = Value::kString;
            ok = String(out.str);
        } else if (Lit(L"true")) {
            out.type = Value::kBool;
            out.b = true;
            ok = true;
        } else if (Lit(L"false")) {
            out.type = Value::kBool;
            out.b = false;
            ok = true;
        } else if (Lit(L"null")) {
            out.type = Value::kNull;
            ok = true;
        } else {
            ok = Number(out);
        }
        --depth;
        return ok;
    }
};

void Escape(const std::wstring& s, std::wstring& out) {
    out.push_back(L'"');
    for (wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back(L'"');
}

void Num(double d, std::wstring& out) {
    wchar_t buf[64];
    // 先判有限性：`1e999` 能解析成 inf（strtod 不报错），而
    // `static_cast<long long>(inf)` 是 UB。inf/nan 在 JSON 里没有字面量，
    // 写成 0 是唯一安全选择（写入方本身就不该产生这种值）。
    if (!std::isfinite(d)) {
        out += L'0';
        return;
    }
    // 只有确认在 long long 范围内才能转 —— `1e300` 同样越界。
    // 上界必须写 2^63 而**不是** `static_cast<double>(LLONG_MAX)`：后者在 double
    // 里也舍入成 2^63，仍会把 d==2^63 放进 static_cast<long long> 触发 UB。
    // 负侧 -2^63 正好可表示，保留闭区间。
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
        d == static_cast<double>(static_cast<long long>(d))) {
        swprintf(buf, 64, L"%lld", static_cast<long long>(d));
    } else {
        swprintf(buf, 64, L"%.10g", d);
    }
    out += buf;
}

void Dump(const Value& v, int indent, int level, std::wstring& out) {
    std::wstring pad(static_cast<size_t>(indent > 0 ? indent * level : 0), L' ');
    std::wstring padIn(static_cast<size_t>(indent > 0 ? indent * (level + 1) : 0), L' ');
    switch (v.type) {
    case Value::kNull: out += L"null"; break;
    case Value::kBool: out += v.b ? L"true" : L"false"; break;
    case Value::kNumber: Num(v.num, out); break;
    case Value::kString: Escape(v.str, out); break;
    case Value::kArray: {
        if (v.arr.empty()) {
            out += L"[]";
            break;
        }
        out += L"[";
        for (size_t k = 0; k < v.arr.size(); ++k) {
            if (k) out += L",";
            if (indent > 0) {
                out += L"\n";
                out += padIn;
            }
            Dump(v.arr[k], indent, level + 1, out);
        }
        if (indent > 0) {
            out += L"\n";
            out += pad;
        }
        out += L"]";
        break;
    }
    case Value::kObject: {
        if (v.obj.empty()) {
            out += L"{}";
            break;
        }
        out += L"{";
        for (size_t k = 0; k < v.obj.size(); ++k) {
            if (k) out += L",";
            if (indent > 0) {
                out += L"\n";
                out += padIn;
            }
            Escape(v.obj[k].first, out);
            out += indent > 0 ? L": " : L":";
            Dump(v.obj[k].second, indent, level + 1, out);
        }
        if (indent > 0) {
            out += L"\n";
            out += pad;
        }
        out += L"}";
        break;
    }
    }
}

}  // namespace

bool Parse(const std::wstring& text, Value& out, std::wstring* err) {
    Parser p(text);
    out = Value{};
    bool ok = p.ValueAt(out);
    if (ok) {
        p.Ws();
        if (p.i != text.size()) {
            p.Fail(L"根值之后还有内容");
            ok = false;
        }
    }
    if (!ok && err) *err = p.err;
    return ok;
}

std::wstring Stringify(const Value& v, int indent) {
    std::wstring out;
    Dump(v, indent, 0, out);
    return out;
}

}  // namespace json
