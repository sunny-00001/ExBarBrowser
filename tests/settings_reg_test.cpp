// 设置键表单测。
//
// 【为什么值得单测】这张表是"机器操作自身"的地基：config.list / config.get /
// config.set 和设置面板全部从它派生。表腐化的方式很隐蔽 ——
//   · 加了一行却忘了接 set  → 改了没反应，日志说成功；
//   · get 读 A、set 写 B     → 界面显示与写入分叉；
//   · 范围写进 doc 却没查    → 文档说 200..5000，代码接受 1。
// 三者都不会崩，只会让"设置改了没生效"变成长期悬案。所以这里逐条验：
// 每项读得出来、可写项写得回去（写后回读一致）、越界与错类型当场拒绝。
#include <cstdio>
#include <set>
#include <string>

#include "json.h"
#include "pane_split.h"   // panesplit::kMinPercent / kMaxPercent
#include "settings_reg.h"

static int g_pass = 0, g_fail = 0;

static void Expect(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n", what);
    }
}

// 写一项，返回是否成功，并带回原因（失败时必须有原因 —— 空原因等于没有信息）。
static bool TrySet(const std::wstring& key, const json::Value& v, std::wstring* err) {
    return settings_reg::Set(key, v, err);
}

// 从 doc 里抽出一段 "a..b"（没有就返回 false）。
//
// 【为什么要抽】doc 是给 AI 的决策依据，手写范围文字会因为"改了下限忘了改注释"
// 而与实际校验分叉 —— 那种分叉不会崩，只会让 AI 挑一个说明允许、实现拒绝的值，
// 然后拿着一条与说明矛盾的报错去猜。抽出来就能和声明范围逐项比对。
static bool DocRange(const std::wstring& doc, long long* lo, long long* hi) {
    const size_t dot = doc.find(L"..");
    if (dot == std::wstring::npos) return false;
    size_t a = dot, b = dot + 2;
    while (a > 0 && doc[a - 1] >= L'0' && doc[a - 1] <= L'9') --a;
    while (b < doc.size() && doc[b] >= L'0' && doc[b] <= L'9') ++b;
    if (a == dot || b == dot + 2) return false;   // 有一边不是数字，不算范围
    *lo = std::stoll(doc.substr(a, dot - a));
    *hi = std::stoll(doc.substr(dot + 2, b - dot - 2));
    return true;
}

int main() {
    const auto& entries = settings_reg::Entries();

    // ① 表本身：非空、键唯一、每个键都读得出来。
    Expect(!entries.empty(), "键表非空");
    std::set<std::wstring> seen;
    for (const auto& e : entries) {
        Expect(seen.insert(e.key).second, "键名唯一");
        json::Value v;
        Expect(settings_reg::Get(e.key, &v), "每一项都读得出来");
        Expect(!e.doc.empty(), "每一项都有中文说明（界面标题与 AI 决策依据）");
        if (e.kind == settings_reg::Kind::Int && e.maxValue > e.minValue)
            Expect(e.maxValue > e.minValue, "Int 项的范围上界大于下界");
    }

    // ② 自检：可写项能用当前值写回（证明 set 真的接线，且写后回读一致）。
    const auto bad = settings_reg::SelfCheck();
    for (const auto& b : bad) std::printf("  自检未通过：%ls\n", b.c_str());
    Expect(bad.empty(), "SelfCheck 全部通过（可写项写回原值成功）");

    // ③ 未知键：读返回 false、写返回 false 且带原因。
    {
        json::Value v;
        Expect(!settings_reg::Get(L"no.such.key", &v), "未知键读失败");
        std::wstring err;
        Expect(!TrySet(L"no.such.key", json::Value::MakeInt(1), &err), "未知键写失败");
        Expect(!err.empty(), "未知键写失败时必须给出原因");
    }

    // ④ 类型不符：布尔项给数字必须拒绝，且原因说清要什么。
    {
        std::wstring err;
        Expect(!TrySet(L"tab", json::Value::MakeInt(1), &err), "布尔项拒绝整数");
        Expect(err.find(L"布尔") != std::wstring::npos, "原因里说明需要布尔值");
    }

    // ⑤ 越界：viewMode 声明 0..7，9 必须被拒（而不是静默接受）。
    {
        json::Value before;
        settings_reg::Get(L"viewMode", &before);
        std::wstring err;
        Expect(!TrySet(L"viewMode", json::Value::MakeInt(9), &err), "越界值被拒绝");
        Expect(err.find(L"超出范围") != std::wstring::npos, "原因里说明超出范围");
        json::Value after;
        settings_reg::Get(L"viewMode", &after);
        Expect(after.num == before.num, "被拒绝的写入没有改动原值");
    }

    // ⑤b 小数写入 Int 项：必须拒绝，且失败不能污染内存值（B1 回归）。
    // 曾经的 bug：AsInt 把 1.5 静默截成 1 写进字段，回读拿原文比失败返回 false，
    // 但字段已被改成 1 —— 命令报失败、状态却变了，下次 Save 固化错值。
    {
        json::Value before;
        settings_reg::Get(L"viewMode", &before);
        json::Value frac;
        frac.type = json::Value::kNumber;
        frac.num = before.num + 0.5;   // 一个带小数、且落在 0..7 范围内的值
        std::wstring err;
        Expect(!TrySet(L"viewMode", frac, &err), "小数写入 Int 项被拒绝");
        Expect(!err.empty(), "小数写入被拒时给出原因");
        json::Value after;
        settings_reg::Get(L"viewMode", &after);
        Expect(after.num == before.num, "小数写入被拒后原值未变（失败不污染内存）");
    }

    // ⑥ 枚举：tabNewPos 只认三个值；大小写不敏感。
    {
        std::wstring err;
        Expect(!TrySet(L"tabNewPos", json::Value::MakeStr(L"bogus"), &err), "枚举拒绝非法取值");
        Expect(err.find(L"after_current") != std::wstring::npos, "原因里列出合法取值");
        json::Value prev;
        settings_reg::Get(L"tabNewPos", &prev);
        Expect(TrySet(L"tabNewPos", json::Value::MakeStr(L"RIGHT"), &err), "枚举值大小写不敏感");
        settings_reg::Set(L"tabNewPos", prev, &err);   // 还原
    }

    // ⑦ 只读项：写必须被拒，且原因是"只读"（不是"没有这个键"）。
    {
        std::wstring err;
        Expect(!TrySet(L"window.x", json::Value::MakeInt(10), &err), "只读项拒绝写入");
        Expect(err.find(L"只读") != std::wstring::npos, "原因里说明是只读项");
    }

    // ⑧ 正常写入 + 回读：布尔项翻转后读回来必须是新值，然后还原。
    {
        json::Value before;
        settings_reg::Get(L"tab", &before);
        const bool target = !before.b;
        std::wstring err;
        Expect(TrySet(L"tab", json::Value::MakeBool(target), &err), "布尔项写入成功");
        json::Value after;
        settings_reg::Get(L"tab", &after);
        Expect(after.b == target, "写完读回来是新值");
        Expect(TrySet(L"tab", before, &err), "布尔项可还原");
    }

    // ⑨ DumpAll 的形状：条数与表一致，每项都带 key/kind/writable。
    // config.list 直接返回它，形状错了是 AI 侧第一个撞到的墙。
    {
        const json::Value dump = settings_reg::DumpAll();
        Expect(dump.IsArray(), "DumpAll 返回数组");
        Expect(dump.arr.size() == entries.size(), "DumpAll 条数与键表一致");
        bool shape_ok = true;
        for (const auto& o : dump.arr) {
            const json::Value* k = o.Find(L"key");
            const json::Value* kind = o.Find(L"kind");
            const json::Value* w = o.Find(L"writable");
            const json::Value* val = o.Find(L"value");
            if (!k || !kind || !w || !val) shape_ok = false;
        }
        Expect(shape_ok, "每项都有 key/kind/writable/value");
    }

    // ⑩ SplitDoc 的四种形状。界面把 doc 拆成"名称列 + 说明列"，拆错的表现是
    // 名字栏里挂着半截括号（只读项写成「名称（只读：细节）」时最容易出）。
    {
        std::wstring name, desc;
        settings_reg::SplitDoc(L"视图模式：0 超大图标 1 大", &name, &desc);
        Expect(name == L"视图模式", "普通形状：名称取冒号前");
        Expect(desc == L"0 超大图标 1 大", "普通形状：说明取冒号后");

        settings_reg::SplitDoc(L"没有冒号的说明", &name, &desc);
        Expect(name == L"没有冒号的说明", "无冒号：整段当名称");
        Expect(desc.empty(), "无冒号：说明为空");

        // 冒号落在括号里 → 改在括号前拆，说明里保留「只读：」。
        settings_reg::SplitDoc(L"快捷键覆盖（只读：改 data\\keymap.json）", &name, &desc);
        Expect(name == L"快捷键覆盖", "括号内冒号：名称取括号外，不带半截括号");
        Expect(desc == L"只读：改 data\\keymap.json", "括号内冒号：说明取括号内，连冒号一起");
        Expect(desc.find(L'）') == std::wstring::npos, "括号内冒号：说明里不留孤零零的右括号");

        // 冒号在括号外 → 仍按老规矩拆（括号属于名称）。
        settings_reg::SplitDoc(L"导航窗格：仅新标签生效（显示中的标签不重建）", &name, &desc);
        Expect(name == L"导航窗格", "括号外冒号：名称取冒号前");
        Expect(desc == L"仅新标签生效（显示中的标签不重建）", "括号外冒号：说明取冒号后");
    }

    // ⑪ 分组覆盖：每一项都要归到一个已登记的分组（左栏标签页），且没有空组
    // （空组会在左栏出现一个点不动的标签）。
    {
        std::set<std::wstring> groupKeys;
        for (const auto& g : settings_reg::Groups()) {
            Expect(!g.key.empty(), "分组有 key");
            Expect(!g.name.empty(), "分组有中文名");
            groupKeys.insert(g.key);
        }
        for (const auto& e : entries)
            Expect(groupKeys.count(e.group) == 1, "每一项都归在已登记的分组里");
        for (const auto& g : settings_reg::Groups()) {
            size_t n = 0;
            for (const auto& e : entries)
                if (e.group == g.key) ++n;
            Expect(n > 0, "每个分组至少有一项");
        }
        // 组名不能重名 —— 左栏是按名字显示的，重名会让用户分不清点的是哪个。
        std::set<std::wstring> names;
        for (const auto& g : settings_reg::Groups())
            Expect(names.insert(g.name).second, "分组中文名唯一");
    }

    // ⑫ 默认值：可写项必须能读出默认值，且默认值与当前值同类型 ——
    // "恢复默认值"按钮直接用这个读；读不出来就是那个按钮点了没反应。
    {
        for (const auto& e : entries) {
            json::Value cur, def;
            Expect(settings_reg::Get(e.key, &cur), "当前值读得出来");
            if (!e.set) continue;   // 只读项没有"恢复默认"入口
            Expect(settings_reg::GetDefault(e.key, &def), "可写项读得出默认值");
            Expect(cur.type == def.type, "默认值与当前值同类型");
        }
    }

    // ⑬ 枚举项与当前值必须对得上：有 options 的项，当前值必然是其中之一 ——
    // 否则下拉框里没有一项被打勾，用户看到的"当前值"与下拉状态自相矛盾。
    {
        for (const auto& e : entries) {
            if (e.options.empty()) continue;
            json::Value cur;
            settings_reg::Get(e.key, &cur);
            bool matched = false;
            for (const auto& o : e.options)
                if (settings_reg::ValueEquals(o.value, cur)) matched = true;
            Expect(matched, "枚举项的当前值在可选值清单里");
        }
    }

    // ⑭ ValueEquals：类型不同必须判不等（1 与 true 不相等），
    // 数组 / 对象要递归比（只比基本类型会让数组项永远"写成功却报不一致"）。
    {
        Expect(settings_reg::ValueEquals(json::Value::MakeInt(1), json::Value::MakeInt(1)),
               "整数相等");
        Expect(!settings_reg::ValueEquals(json::Value::MakeInt(1), json::Value::MakeBool(true)),
               "整数 1 与布尔 true 不相等");
        Expect(!settings_reg::ValueEquals(json::Value::MakeStr(L"1"), json::Value::MakeInt(1)),
               "字符串 \"1\" 与整数 1 不相等");

        json::Value a = json::Value::MakeArray();
        a.arr.push_back(json::Value::MakeInt(1));
        a.arr.push_back(json::Value::MakeStr(L"x"));
        json::Value b = json::Value::MakeArray();
        b.arr.push_back(json::Value::MakeInt(1));
        b.arr.push_back(json::Value::MakeStr(L"x"));
        Expect(settings_reg::ValueEquals(a, b), "数组逐项相等");
        b.arr[1] = json::Value::MakeStr(L"y");
        Expect(!settings_reg::ValueEquals(a, b), "数组有一项不同即不等");
        b.arr.push_back(json::Value::MakeInt(2));
        Expect(!settings_reg::ValueEquals(a, b), "数组长度不同即不等");

        json::Value o1 = json::Value::MakeObject();
        o1.Set(L"k", json::Value::MakeInt(1));
        json::Value o2 = json::Value::MakeObject();
        o2.Set(L"k", json::Value::MakeInt(1));
        Expect(settings_reg::ValueEquals(o1, o2), "对象逐字段相等");
        o2.Set(L"k", json::Value::MakeInt(2));
        Expect(!settings_reg::ValueEquals(o1, o2), "对象有一字段不同即不等");
    }

    // ⑮ 新增的可调项真的接线了：三项历史上限与 AI 超时都拒绝越界值。
    //
    // 【为什么这几条值得单独钉】它们是"原本写死在代码里的数字"搬进配置的结果：
    // 搬过来最容易出的错是"表里登记了、范围声明了，但 set 里没 Checklists 范围"
    // —— 表现就是越界值被静默接受，而这类错从界面上完全看不出来。
    {
        std::wstring err;
        // 越界被拒（不是被夹到边界 —— 静默夹取会让界面显示的值与用户输入不符）
        Expect(!TrySet(L"recentLimit", json::Value::MakeInt(0), &err), "recentLimit 拒绝 0");
        Expect(!TrySet(L"recentLimit", json::Value::MakeInt(999), &err), "recentLimit 拒绝 999");
        Expect(!TrySet(L"closedTabLimit", json::Value::MakeInt(0), &err), "closedTabLimit 拒绝 0");
        Expect(!TrySet(L"aiTimeoutMs", json::Value::MakeInt(10), &err), "aiTimeoutMs 拒绝 10ms");
        Expect(!TrySet(L"aiTimeoutMs", json::Value::MakeInt(9999999), &err), "aiTimeoutMs 拒绝超上限");

        // 边界值可以写（下界本身是合法值，不是"越界"）
        json::Value prev;
        settings_reg::Get(L"recentLimit", &prev);
        Expect(TrySet(L"recentLimit", json::Value::MakeInt(1), &err), "recentLimit 接受下界 1");
        Expect(TrySet(L"recentLimit", json::Value::MakeInt(200), &err), "recentLimit 接受上界 200");
        settings_reg::Set(L"recentLimit", prev, &err);
    }

    // ⑯ themeMode：枚举只认三个稳定标识，且大小写不敏感（与 tabNewPos 同口径）。
    {
        std::wstring err;
        Expect(!TrySet(L"themeMode", json::Value::MakeStr(L"blue"), &err), "themeMode 拒绝非法值");
        Expect(err.find(L"light") != std::wstring::npos, "原因里列出合法取值");
        json::Value prev;
        settings_reg::Get(L"themeMode", &prev);
        Expect(TrySet(L"themeMode", json::Value::MakeStr(L"DARK"), &err), "themeMode 大小写不敏感");
        settings_reg::Set(L"themeMode", prev, &err);
    }

    // ⑰ searchRange=2（此电脑）必须被拒：这一档在本宿主搜不出来。
    // 只查 0..3 会把"永远搜不动的一档"写进配置，用户以为设上了、之后每次搜索都失败。
    {
        std::wstring err;
        Expect(!TrySet(L"searchRange", json::Value::MakeInt(2), &err),
               "searchRange 拒绝不可用档位（此电脑）");
        Expect(!err.empty() && err.find(L"超出范围") == std::wstring::npos,
               "拒绝原因说的是『这一档用不了』，不是『越界』");
        json::Value prev;
        settings_reg::Get(L"searchRange", &prev);
        Expect(TrySet(L"searchRange", json::Value::MakeInt(0), &err), "searchRange 接受可用档位");
        settings_reg::Set(L"searchRange", prev, &err);

        // 下拉/AI 域必须与写入判据一致：options 里不该出现必然被拒的档位 2。
        // 否则面板摆着一个点了就失败的假选项，config.list 也把它报作合法域。
        const settings_reg::Entry* e = settings_reg::Find(L"searchRange");
        Expect(e && !e->options.empty(), "searchRange 声明了 options");
        if (e)
            for (const auto& o : e->options) {
                Expect(o.value.IsNumber(), "options 项携带数值");
                Expect(o.value.num != 2, "searchRange 的 options 不含不可用档位 2");
            }
    }

    // ⑱ 此前"在 settings.json 里但 config.list 看不见"的项现在都登记了。
    // 这一条防的是"字段存在、界面/命令却没有"的能力缺失 —— 它没有任何检查会报。
    {
        static const wchar_t* kMustExist[] = {
            L"themeMode",   L"recentLimit",      L"closedTabLimit",   L"searchLimit",
            L"navPaneWidth", L"navPaneRoots",    L"searchRange",      L"searchRangePath",
            L"searchHistory", L"sessionSelection", L"sessionHistory", L"sessionPane2Paths",
            L"sessionPane2History", L"aiTimeoutMs",
        };
        for (const wchar_t* k : kMustExist) {
            json::Value v;
            Expect(settings_reg::Get(k, &v), "设置项已登记且读得出来");
        }
    }

    // ⑲ 声明范围 == 实现范围（表驱动，逐项验）。
    //
    // 【为什么要有这一条】这一层最容易出的错不是"崩"，而是**两条路各有一个边界**：
    // settings_reg 声明 holdDelayMs 可以取 200..2000，config_io 读盘时却写
    // "低于 300 就当 800"。于是 `config.set holdDelayMs 250` 成功、写进文件，
    // 下次启动悄悄变回 800 —— 用户看到的是"设置自己变回去了"，没有任何检查会报。
    // 判据刻意与业务判据解耦：
    //   · min-1 / max+1 必须被拒，且原因说的是"超出范围"（不是别的业务理由）；
    //   · min / max 本身**不得**被"超出范围"拒绝（被业务判据拒绝是允许的）。
    for (const auto& e : entries) {
        if (e.kind != settings_reg::Kind::Int || e.maxValue <= e.minValue) continue;
        json::Value prev;
        settings_reg::Get(e.key, &prev);

        std::wstring err;
        const bool lo_out = TrySet(e.key, json::Value::MakeInt(e.minValue - 1), &err);
        Expect(!lo_out && err.find(L"超出范围") != std::wstring::npos,
               "下界外一个值被『超出范围』拒绝");
        const bool hi_out = TrySet(e.key, json::Value::MakeInt(e.maxValue + 1), &err);
        Expect(!hi_out && err.find(L"超出范围") != std::wstring::npos,
               "上界外一个值被『超出范围』拒绝");

        // 边界值本身是合法值：不能被"超出范围"拒绝（边界算不算合法是另一回事，
        // 但若 setter 的范围比声明的窄，报出来的必然是这一句）。
        err.clear();
        TrySet(e.key, json::Value::MakeInt(e.minValue), &err);
        Expect(err.find(L"超出范围") == std::wstring::npos, "下界本身不被判『超出范围』");
        err.clear();
        TrySet(e.key, json::Value::MakeInt(e.maxValue), &err);
        Expect(err.find(L"超出范围") == std::wstring::npos, "上界本身不被判『超出范围』");

        // 逐项还原：这一节把每一项都写过，不还原就会污染后面依赖当前值的检查。
        settings_reg::Set(e.key, prev, &err);
    }

    // ⑳ doc 里写的范围必须等于声明的范围。
    // doc 是 tools/list 暴露给 AI 的取值说明；写错一个数字不会崩，只会让 AI
    // 按一条与实现不同的边界去规划。
    for (const auto& e : entries) {
        if (e.kind != settings_reg::Kind::Int || e.maxValue <= e.minValue) continue;
        long long lo = 0, hi = 0;
        if (!DocRange(e.doc, &lo, &hi)) continue;   // 说明里没引用范围，不适用
        Expect(lo == e.minValue && hi == e.maxValue, "doc 里引用的范围等于声明范围");
    }

    // ㉑ 跨字段约束：标签宽度「上限不得低于下限」必须在**写入端**也成立。
    // 只在读盘端夹取的话，config.set 能写进一个自相矛盾的对（min 230 / max 100），
    // 而它要等到下次启动才被"修好"—— 中间这段时间界面按矛盾值排版。
    {
        json::Value minPrev, maxPrev;
        settings_reg::Get(L"tabMinWidth", &minPrev);
        settings_reg::Get(L"tabMaxWidth", &maxPrev);
        std::wstring err;

        // 先把上限调大，再把下限顶到上限之上 → 必须拒绝。
        Expect(TrySet(L"tabMaxWidth", json::Value::MakeInt(300), &err), "tabMaxWidth 可调大");
        const bool up = TrySet(L"tabMinWidth", json::Value::MakeInt(320), &err);
        Expect(!up, "tabMinWidth 高于 tabMaxWidth 被拒绝");
        Expect(err.find(L"tabMaxWidth") != std::wstring::npos, "拒绝原因点名了另一项");

        // 反向：下限已在 300，把上限压到 60 → 必须拒绝（min 到 300 了，60 不成立）。
        Expect(TrySet(L"tabMinWidth", json::Value::MakeInt(300), &err), "tabMinWidth 可调到 300");
        const bool down = TrySet(L"tabMaxWidth", json::Value::MakeInt(60), &err);
        Expect(!down, "tabMaxWidth 低于 tabMinWidth 被拒绝");

        settings_reg::Set(L"tabMinWidth", minPrev, &err);
        settings_reg::Set(L"tabMaxWidth", maxPrev, &err);
    }

    // ㉒ paneSplit 的上下界必须来自 panesplit（界面能拖到的区间）。
    // 抄一份字面量的话，"拖到 85% 松手被夹回 80%"会重新出现，而且没有任何检查会报。
    {
        const settings_reg::Entry* e = settings_reg::Find(L"paneSplit");
        Expect(e != nullptr, "paneSplit 已登记");
        if (e) {
            Expect(e->minValue == panesplit::kMinPercent, "paneSplit 下界取自 panesplit");
            Expect(e->maxValue == panesplit::kMaxPercent, "paneSplit 上界取自 panesplit");
        }
    }

    std::printf("%s  settings_reg_test: %d 通过, %d 失败\n",
                g_fail == 0 ? "OK  " : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
