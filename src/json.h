// 极简 JSON：只为读 data/settings.json、keymap.json、mouse.json、bookmarks.html。
//
// 刻意不做的事：流式、注释、数字精度保持、NaN。够用即可，出错带行号。
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { kNull = 0, kBool, kNumber, kString, kArray, kObject };

    Type type = kNull;
    bool b = false;
    double num = 0.0;
    std::wstring str;
    std::vector<Value> arr;
    std::vector<std::pair<std::wstring, Value>> obj;

    bool IsNull() const { return type == kNull; }
    bool IsArray() const { return type == kArray; }
    bool IsObject() const { return type == kObject; }
    bool IsString() const { return type == kString; }

    const Value* Find(const wchar_t* key) const;
    std::wstring GetStr(const wchar_t* key, const std::wstring& def = L"") const;
    bool GetBool(const wchar_t* key, bool def = false) const;
    int GetInt(const wchar_t* key, int def = 0) const;

    void Set(const std::wstring& key, Value v);

    static Value MakeStr(const std::wstring& s);
    static Value MakeBool(bool v);
    static Value MakeInt(int v);
    // 大整数（文件大小常超过 2 GB）。内部用 double 存，能精确表示到 2^53
    // （约 9 PB）—— 对文件大小足够，且 Stringify 会输出成整数形式而不是 1e+10。
    static Value MakeNum(double v);
    static Value MakeArray();
    static Value MakeObject();
};

bool Parse(const std::wstring& text, Value& out, std::wstring* err = nullptr);
std::wstring Stringify(const Value& v, int indent = 2);

}  // namespace json
