// settings_reg 的接口：读、写、枚举、自检。
//
// 【表本体在另一个文件】"有哪些项"（settings_reg_table.cpp）与"这一项能不能
// 读写"（这里）是两件事，改动节奏不同。表本体提供数据，这里提供语义：
// 类型不符当场拒绝、写后回读比对、默认值从同一份映射里读。
//
// 【写后回读】赋值语句跑过 ≠ 状态真的变了。三种翻车能被这一步抓住：
//   · 写了 A 字段、读取却读 B 字段（命名漂移）；
//   · 值被截断/归一（把 5000 存成 500）；
//   · get 与 set 指向不同副本（两份状态）。
// 静默成功是设置类 bug 最贵的形态：改了、界面没变，日志却说成功。
#include "settings_reg.h"

#include "config.h"
#include "settings_reg_table.h"   // 表本体
#include "str.h"

namespace settings_reg {
namespace {

// 默认值来源：一份默认构造的 Settings。
//
// 【为什么不是"另抄一份默认值"】另抄的那份迟早与 Settings 的成员初值分叉，
// 而分叉的表现是"按了恢复默认值，界面跳到另一个值"。这里直接用同一份
// 声明 —— 改 Settings 的默认值，这一处跟着改。
const Settings& Defaults() {
    static const Settings d;
    return d;
}

// 类型名，用于错误信息里说"收到的是什么"。
const wchar_t* TypeName(const json::Value& v) {
    switch (v.type) {
    case json::Value::kNull: return L"null";
    case json::Value::kBool: return L"bool";
    case json::Value::kNumber: return L"number";
    case json::Value::kString: return L"string";
    case json::Value::kArray: return L"array";
    case json::Value::kObject: return L"object";
    }
    return L"?";
}

// 值取自哪一份 Settings：运行值还是默认值。
const Settings& Source(bool useDefault) { return useDefault ? Defaults() : Config::Get().s(); }

// 读一项（运行值或默认值）。取不到 = key 不存在或该项没有读入口。
bool GetFrom(const std::wstring& key, bool useDefault, json::Value* out) {
    const Entry* e = Find(key);
    if (!e || !out || !e->get) return false;
    *out = e->get(Source(useDefault));
    return true;
}

}  // namespace

// 值比较（公开接口，说明见上）。只比同类型基本值，不做隐式转换
// （1 与 true 相等是最该报出来的错，而不是最该放过的情况）。
//
// 【为什么是公开接口】界面判断"当前值是不是某个枚举项"用的是同一个问题。
// 私有一份、界面再写一份，迟早在"数字 5 与字符串 "5""这种边界上给出不同答案 ——
// 而那种分歧只表现为下拉里勾错了项。
bool ValueEquals(const json::Value& a, const json::Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case json::Value::kBool: return a.b == b.b;
    case json::Value::kNumber: return a.num == b.num;
    case json::Value::kString: return a.str == b.str;
    // 数组/对象必须递归比：statusSegs 变成可写后，写后回读若仍只比基本类型，
    // 任何数组项都会"写成功了却报回读不一致"（SelfCheck 会一条条报红）。
    case json::Value::kArray:
        if (a.arr.size() != b.arr.size()) return false;
        for (size_t i = 0; i < a.arr.size(); ++i)
            if (!ValueEquals(a.arr[i], b.arr[i])) return false;
        return true;
    case json::Value::kObject:
        if (a.obj.size() != b.obj.size()) return false;
        for (const auto& kv : a.obj) {
            const json::Value* other = b.Find(kv.first.c_str());
            if (!other || !ValueEquals(kv.second, *other)) return false;
        }
        return true;
    case json::Value::kNull: return true;
    }
    return false;
}

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Bool: return "bool";
    case Kind::Int: return "int";
    case Kind::Str: return "string";
    case Kind::StrArray: return "string[]";
    case Kind::IntArray: return "int[]";
    case Kind::ObjArray: return "object[]";
    case Kind::Object: return "object(readonly)";
    }
    return "?";
}

const std::vector<Entry>& Entries() { return SettingsTable(); }

const std::vector<Group>& Groups() { return SettingsGroupTable(); }

const Group* FindGroup(const std::wstring& key) {
    for (const auto& g : Groups())
        if (g.key == key) return &g;
    return nullptr;
}

const Entry* Find(const std::wstring& key) {
    for (const auto& e : Entries())
        if (e.key == key) return &e;
    return nullptr;
}

bool Get(const std::wstring& key, json::Value* out) { return GetFrom(key, false, out); }

bool GetDefault(const std::wstring& key, json::Value* out) { return GetFrom(key, true, out); }

bool Set(const std::wstring& key, const json::Value& value, std::wstring* err) {
    const Entry* e = Find(key);
    if (!e) {
        if (err) *err = L"没有这个设置项：" + key;
        return false;
    }
    if (!e->set) {
        // 只读项：明确说"只读"并给出该走哪个入口，别让调用方以为是名字写错了。
        if (err) *err = key + L" 是只读项（" + e->doc + L"）";
        return false;
    }
    std::wstring why;
    if (!e->set(value, &why)) {
        if (err) {
            *err = why.empty() ? (key + L" 不接受该值（收到 " + TypeName(value) + L"）") : why;
        }
        return false;
    }

    // 写后回读（理由见文件头）。
    json::Value back;
    if (!Get(key, &back)) {
        if (err) *err = key + L" 写入后读不回来（get 与 set 不在同一份状态上）";
        return false;
    }
    // 期望值是**规范化之后**的形状：有些项会故意把请求转成规范形再存
    // （旧别名 right → end、路径分隔符归一），拿原文去比会把正确的写入判成失败。
    // 没声明 canon 的项一律严格比原文。
    const json::Value expect = e->canon ? e->canon(value) : value;
    if (!ValueEquals(back, expect)) {
        if (err)
            *err = key + L" 写入后回读不一致：期望 " + json::Stringify(expect, 0) + L"，实际 " +
                   json::Stringify(back, 0);
        return false;
    }
    return true;
}

json::Value DumpAll() {
    json::Value items = json::Value::MakeArray();
    for (const auto& e : Entries()) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"key", json::Value::MakeStr(e.key));
        o.Set(L"kind", json::Value::MakeStr(str::FromUtf8(KindName(e.kind))));
        o.Set(L"doc", json::Value::MakeStr(e.doc));
        o.Set(L"group", json::Value::MakeStr(e.group));
        o.Set(L"writable", json::Value::MakeBool(static_cast<bool>(e.set)));
        if (e.get) o.Set(L"value", e.get(Config::Get().s()));
        // 范围只对 Int 有意义；未声明范围时不给 min/max，免得调用方以为
        // "0..0" 是真实取值域。
        if (e.kind == Kind::Int && e.maxValue > e.minValue) {
            o.Set(L"min", json::Value::MakeInt(e.minValue));
            o.Set(L"max", json::Value::MakeInt(e.maxValue));
        }
        // 枚举项连**取值**一起给：只给中文名，调用方无法把"详细信息"翻回 5。
        if (!e.options.empty()) {
            json::Value opts = json::Value::MakeArray();
            for (const auto& opt : e.options) {
                json::Value oo = json::Value::MakeObject();
                oo.Set(L"label", json::Value::MakeStr(opt.label));
                oo.Set(L"value", opt.value);
                opts.arr.push_back(std::move(oo));
            }
            o.Set(L"options", std::move(opts));
        }
        items.arr.push_back(std::move(o));
    }
    return items;
}

namespace {

// get 返回的形状是否与声明的 Kind 一致。
//
// 【为什么要查】Kind 是**对外契约**：config.list 把 KindName 报给 AI 与脚本，
// 界面按它决定"这个值怎么显示、怎么编辑"。声明成 Int 而 get 返回字符串时，
// 契约就是假的 —— 调用方按数字去取会拿到空，界面画出一个填不进去的数字框。
// 这种错在编译期完全看不出来（get 是 std::function，返回 json::Value）。
bool ShapeMatches(Kind k, const json::Value& v) {
    switch (k) {
    case Kind::Bool: return v.IsBool();
    case Kind::Int: return v.IsNumber();
    case Kind::Str: return v.IsString();
    case Kind::ObjArray: return v.IsArray();
    case Kind::Object: return v.IsObject();
    case Kind::StrArray:
        if (!v.IsArray()) return false;
        for (const auto& e : v.arr)
            if (!e.IsString()) return false;
        return true;
    case Kind::IntArray:
        if (!v.IsArray()) return false;
        for (const auto& e : v.arr)
            if (!e.IsNumber()) return false;
        return true;
    }
    return false;
}

}  // namespace

std::vector<std::wstring> SelfCheck() {
    std::vector<std::wstring> bad;
    for (const auto& e : Entries()) {
        json::Value cur;
        if (!Get(e.key, &cur)) {
            bad.push_back(e.key + L"：读不出来（get 为空或实现有误）");
            continue;
        }
        // 分组必须已登记：界面按分组切段落，分组没登记这一项就永远显示不出来，
        // 而"表里有、界面没有"是那种没有任何检查会报的能力缺失。
        if (e.group.empty() || !FindGroup(e.group)) {
            bad.push_back(e.key + L"：分组未登记（group=" + e.group + L"）");
        }
        // doc 必须能拆出"说明"（见 SplitDoc）：界面把它拆成名称列 + 说明列，
        // 没有全角冒号时说明列永远是空的 —— 而"这一项的说明是空白"不属于任何
        // 会被人报上来的现象，它只是看上去"这软件没那么细"。
        if (e.doc.find(L'：') == std::wstring::npos) {
            bad.push_back(e.key + L"：doc 没有全角冒号（界面名称/说明两列会拆不开）");
        }
        // 形状与声明一致：Kind 是给 AI/脚本看的契约，错了它就会按错的类型取值。
        if (!ShapeMatches(e.kind, cur)) {
            bad.push_back(e.key + L"：声明的类型（" + str::FromUtf8(KindName(e.kind)) +
                          L"）与 get 返回的形状不符");
        }

        if (!e.set) {
            // 只读项必须自己说明"为什么只读、该走哪个入口"。
            //
            // 【为什么值得一条检查】只读项写不进去，用户/脚本看到的都是"改了没反应"。
            // doc 里没有"只读"两个字时，界面上它长得和可写项一模一样（同样一行、
            // 同样的标题），没人知道它不能改 —— 而这不是 bug 报告会提到的事。
            if (e.doc.find(L"只读") == std::wstring::npos) {
                bad.push_back(e.key + L"：只读项但 doc 没说明只读（应当在说明里写清入口）");
            }
            continue;
        }

        // 可写的整型项必须声明数值域。
        //
        // 【为什么】没声明就等于"任何 int 都接受"：负数宽度、0 毫秒超时都会被
        // 写进配置，然后由使用处各自夹取 —— 那正是"同一条规则有 N 份实现"的开端。
        // 显式声明之后，越界值在这里就被拒，使用处不必再猜。
        if (e.kind == Kind::Int && e.maxValue <= e.minValue) {
            bad.push_back(e.key + L"：可写的整型项没有声明数值域（min/max）");
        }

        // 写回原值：既不改变状态，又能证明 set 真的接线了。
        std::wstring err;
        if (!Set(e.key, cur, &err)) bad.push_back(e.key + L"：写回原值失败（" + err + L"）");

        // 声明的数值域与 set 真实接受的区间必须一致 —— **两个方向都试**。
        //
        // 【为什么要真的写一次】"查了范围"和"声明了范围"是两件事，只有前者在
        // 写入口生效。不试一次的话，"表里写 200..2000、set 里没查"永远查不出来。
        //
        // 【为什么两个方向都要】只试越界值只能抓到"set 更宽"；"set 更窄"仍然是
        // 静默的（边界值被当成越界拒掉，调用方以为是自己填错了）。实测过：
        // 把 holdDelayMs 的 set 改成 250..1900，只试越界值的版本一条都不报。
        // 所以还要反过来试边界本身：它可以被业务判据拒绝（跨字段约束之类），
        // 但**不能**被"超出范围"拒绝 —— 那就说明声明的范围是假的。
        if (e.kind == Kind::Int && e.maxValue > e.minValue) {
            bool touched = false;
            for (int probe : {e.minValue - 1, e.maxValue + 1}) {
                err.clear();
                if (Set(e.key, json::Value::MakeInt(probe), &err)) {
                    touched = true;
                    bad.push_back(e.key + L"：越界值 " + std::to_wstring(probe) +
                                  L" 被接受（声明范围 " + std::to_wstring(e.minValue) + L".." +
                                  std::to_wstring(e.maxValue) + L"，但 set 没查）");
                }
            }
            for (int probe : {e.minValue, e.maxValue}) {
                err.clear();
                const bool ok = Set(e.key, json::Value::MakeInt(probe), &err);
                if (ok) touched = true;
                if (!ok && err.find(L"超出范围") != std::wstring::npos) {
                    bad.push_back(e.key + L"：边界值 " + std::to_wstring(probe) +
                                  L" 被判超出范围（声明的范围比 set 接受的宽）");
                }
            }
            // 检查本身不该留下痕迹：只要写过就还原。
            if (touched) Set(e.key, cur, &err);
        }
    }
    return bad;
}

void SplitDoc(const std::wstring& doc, std::wstring* name, std::wstring* desc) {
    // 拆点固定是**第一个全角冒号**。半角冒号不算 —— doc 里可能出现
    // "data\\keymap.json" 这类含半角冒号的路径说明，用半角拆会把说明拦腰截断。
    const size_t pos = doc.find(L'：');
    if (pos == std::wstring::npos) {
        if (name) *name = doc;
        if (desc) desc->clear();
        return;
    }

    // 【括号里的冒号】表里有几项写成「名称（只读：细节）」这种形状
    // （窗口几何 / 键位 / AI 通道）。按第一个冒号拆，名字会变成
    // 「名称（只读」—— 名字栏里挂着半截括号，说明栏里也丢了「只读」二字，
    // 而"这项只读"恰恰是最该让人看见的信息。
    //
    // 所以遇到"冒号在括号里"就改在括号前拆：名字取括号外，说明取括号内
    // （连冒号一起，说明栏里就是完整的「只读：细节」）。
    //
    // 判定条件：这个 '（' 的配对 '）' 在**冒号之后** —— 那才说明冒号落在
    // 括号里。反过来「说明（括号）：细节」里冒号在括号外，仍按老规矩拆。
    const size_t open = doc.rfind(L'（', pos);
    const size_t close = doc.find(L'）', pos);
    if (open != std::wstring::npos && close != std::wstring::npos &&
        doc.find(L'）', open) == close) {
        std::wstring d = doc.substr(open + 1);
        // 右括号跟着括号里那段走 —— 说明栏里留一个孤零零的「）」很难看。
        if (!d.empty() && d.back() == L'）') d.pop_back();
        if (name) *name = doc.substr(0, open);
        if (desc) *desc = d;
        return;
    }

    if (name) *name = doc.substr(0, pos);
    if (desc) *desc = doc.substr(pos + 1);
}

}  // namespace settings_reg
