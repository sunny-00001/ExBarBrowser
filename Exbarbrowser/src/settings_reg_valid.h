// settings_reg 的取值校验与写入构造器（键表本体在 settings_reg.cpp）。
//
// 【为什么拆出来】键表回答"有哪些设置项、各自什么形状"；校验与构造回答
// "一个 JSON 值能不能进这个字段、不能时为什么"。两者一起长大时，键表文件会变成
// "一半是清单一半是机制"，读的人要翻到文件中间才知道某个 set 为什么拒绝一个值。
//
// 【口径】刻意不做隐式转换："把 true 当成 1 写进 int 字段"这类静默换算，正是
// "设置改了但没生效"最常见的来源。报错一次，比留一个神秘 bug 好。
//
// 【唯一实现】校验与写入只有这一份实现：键表里每个可写项都从这里取构造器，
// 校验规则不写在 lambda 里 —— "某处忘了查范围"会变成常态，而那种错误的表现是
// "越界值被静默接受"。
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "config.h"   // TabMeta
#include "json.h"

namespace settings_reg {

// 写构造器：把一次 set 调用变成"校验 + 落字段 + 说明原因"。
using Setter = std::function<bool(const json::Value&, std::wstring*)>;

// ---- 类型校验（json 值 -> 设置项字段）----
bool AsBool(const json::Value& v, bool* out);
bool AsInt(const json::Value& v, int* out);
bool AsStr(const json::Value& v, std::wstring* out);
bool AsStrArray(const json::Value& v, std::vector<std::wstring>* out);
bool AsIntArray(const json::Value& v, std::vector<int>* out);

// ---- 写构造器（校验通过后落到字段上）----
// lo == hi == 0 表示不查范围（用于没有明确边界、但必须拒绝非整数的项）。
Setter BoolSet(bool* field, const wchar_t* key);
Setter IntSet(int* field, const wchar_t* key, int lo, int hi);
// 带额外判据的整数项：范围之外再加一条"业务上能不能用"。
//
// 【为什么需要它】searchRange 的取值域是 0..3，但 2（"此电脑"）在本宿主
// 搜不出来（见 search_scope.h）。只查范围就会把"永远搜不动的一档"写进配置，
// 而那种设置躺在那儿、用户以为设上了，之后每次搜索都失败 —— 比当场报错更坑。
// 判据以 lambda 注入而不是在这里 include search_scope.h：本文件是纯校验，
// 不该认识任何一个业务模块。
using AllowedFn = std::function<bool(int)>;
Setter IntCheckedSet(int* field, const wchar_t* key, int lo, int hi, AllowedFn allowed,
                     std::wstring rejectWhy);
Setter StrSet(std::wstring* field, const wchar_t* key);
Setter StrEnumSet(std::wstring* field, const wchar_t* key, std::vector<std::wstring> allowed);

// 字符串项的"规格"：允许的取值 + 规范形。
//
// 【为什么要把两件事绑在一起】有些项会故意把请求规范化再存（tabNewPos 的旧别名
// right → end、自定义搜索范围路径的分隔符归一）。而写入之后要回读校验
// （见 settings_reg::Set 的"写后回读"），判据是"回读 == 请求"—— 规范化过的项
// 每次都会被判成失败。于是 Entry 可以声明 canon（回读期望值），而 canon 必须与
// setter 用**同一个**规范化函数，否则"存的"和"期望的"各有定义。
//
// 把两者收进一个结构体后，调用方只写一次规范化、同时拿到 setter 与 canon：
// 想分叉都做不到。allowed 为空 = 任意字符串（路径这类）。
struct StrSpec {
    std::wstring key;
    std::vector<std::wstring> allowed;
    // 规范形（可为空 = 原样）。接受 std::wstring 而不是 json，是因为
    // "把这个字符串改成规范写法"和 JSON 类型检查是两件事。
    std::function<std::wstring(std::wstring)> norm;

    std::wstring Canonical(std::wstring s) const { return norm ? norm(std::move(s)) : s; }
    // 生成 Entry::set。类型检查与取值白名单都在这里，理由同 StrEnumSet。
    Setter SetterFor(std::wstring* field) const;
    // 生成 Entry::canon。
    std::function<json::Value(const json::Value&)> CanonFor() const;
};
// 整型数组项的"规格"：取值范围 + 规范形。
//
// 【为什么与 StrSpec 同形】问题完全一样：有些数组项会故意把请求规范化再存
// （statusSegs 的去重与升序）。而写入之后要回读校验，判据是"回读 == 请求"——
// 规范化过的项每次都会被判成失败。canon 必须与 setter 用**同一个**规范化函数，
// 否则"存的"和"期望的"各有定义。收进一个结构体后，调用方只写一次。
//
// 范围约定与 IntSet 一致：**闭区间** [lo, hi]。
// （早先 IntArraySet 用的是半开区间 n >= hi 拒绝，与 IntSet 相反 —— 同一份
//   校验文件里两套区间语义，迟早有人按错的语义传参。）
struct IntArraySpec {
    std::wstring key;
    int lo = 0;
    int hi = 0;
    // 规范形（可为空 = 原样）。参数是值而不是 JSON：类型检查与区间检查
    // 是另外两件事，混进规范化里会让"拒绝原因"说不清是哪个环节。
    std::function<std::vector<int>(std::vector<int>)> norm;

    std::vector<int> Canonical(std::vector<int> v) const {
        return norm ? norm(std::move(v)) : v;
    }
    Setter SetterFor(std::vector<int>* field) const;
    std::function<json::Value(const json::Value&)> CanonFor() const;
};

Setter StrArraySet(std::vector<std::wstring>* field, const wchar_t* key);
// 闭区间 [lo, hi]（与 IntSet 一致）。
Setter IntArraySet(std::vector<int>* field, const wchar_t* key, int lo, int hi);

// ---- 读构造器（设置项字段 -> json 值）----
json::Value StrArrayValue(const std::vector<std::wstring>& v);
json::Value IntArrayValue(const std::vector<int>& v);
json::Value TabMetaValue(const std::vector<TabMeta>& v);
json::Value HotkeyValue(const std::vector<std::pair<std::wstring, std::wstring>>& v);
// 每标签导航历史（sessionHistory / sessionPane2History）：导出成
// [{paths:[...], idx:N}] 的形状，与 settings.json 里的写法完全一致 ——
// 两处形状不同就得有个"翻译层"，而翻译层是"读到的和存的不一样"的常见来源。
json::Value TabHistoryValue(const std::vector<TabHistory>& v);

}  // namespace settings_reg
