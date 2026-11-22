// settings_reg 的实现：一份键表 + 一套校验器 + 唯一落字段路径。
//
// 读写的"形状"由表决定，具体字段只出现在两处：get 的 lambda 和 set 构造器
// 收到的指针。新增设置项 = 加一行 + 在 config.cpp 里加读写，别处不用改。
#include "settings_reg.h"

#include "config.h"
#include "status_bar.h"   // Seg_Count：statusSegs 的取值上界
#include "str.h"

namespace settings_reg {
namespace {

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
// 18 个可写项如果各写一个 lambda，"某处忘了查范围"就会成为常态，而那种错误的
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
            if (n < lo || n >= hi) {
                if (err)
                    *err = std::wstring(key) + L" 的元素须在 [" + std::to_wstring(lo) + L", " +
                           std::to_wstring(hi - 1) + L"]，收到 " + std::to_wstring(n);
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

// ---------------------------------------------------------------- 表本体
//
// 排序即文档顺序即界面顺序：按领域分组（栏 / 视图 / 序列键 / 标签 / 窗口 /
// 会话 / 状态栏 / 键位 / AI），组内按"最可能被调的在前"。
const std::vector<Entry>& Table() {
    static const std::vector<Entry> t = [] {
        std::vector<Entry> v;
        Config& cfg = Config::Get();
        Settings& m = cfg.mutableSettings();

        // ---- 栏显隐（立即生效：config.set 之后会 SyncChrome）----
        v.push_back({L"tab", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().showTab); },
                     BoolSet(&m.showTab, L"tab"),
                     L"显示标签栏：关闭后整条隐藏，仍可用快捷键切换标签"});
        v.push_back({L"nav", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().showNav); },
                     BoolSet(&m.showNav, L"nav"),
                     L"显示导航栏：含地址框、后退/前进、搜索框"});
        v.push_back(
            {L"bookmark", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().showBookmark); },
             BoolSet(&m.showBookmark, L"bookmark"),
             L"显示书签栏：与 bmk.show / bmk.hide 命令是同一条状态"});
        v.push_back(
            {L"statusbar", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().showStatus); },
             BoolSet(&m.showStatus, L"statusbar"), L"显示状态栏：底部信息条"});

        // ---- 视图（立即套用到当前标签）----
        v.push_back({L"viewMode", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().viewMode); },
                     IntSet(&m.viewMode, L"viewMode", 0, 7),
                     L"视图模式：0 超大图标 1 大 2 中 3 小 4 列表 5 详细信息 6 平铺 7 内容",
                     0, 7});
        v.push_back({L"sortMode", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().sortMode); },
                     IntSet(&m.sortMode, L"sortMode", 0, 3),
                     L"排序方式：0 名称 1 日期 2 类型 3 大小", 0, 3});
        v.push_back({L"sortAscending", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().sortAscending); },
                     BoolSet(&m.sortAscending, L"sortAscending"), L"升序：false = 降序"});
        v.push_back({L"groupBy", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().groupBy); },
                     IntSet(&m.groupBy, L"groupBy", 0, 3),
                     L"分组方式：0 不分组 1 类型 2 日期 3 大小", 0, 3});

        // 窗格是"新标签套用"：ShellView 在创建时按配置初始化窗格宿主，
        // 显示中的标签没有改这一项的接口。doc 必须说清，否则调完看不到变化。
        v.push_back(
            {L"showNavPane", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().showNavPane); },
             BoolSet(&m.showNavPane, L"showNavPane"), L"导航窗格：仅新标签生效（显示中的标签不重建）"});
        v.push_back({L"showPreviewPane", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().showPreviewPane); },
                     BoolSet(&m.showPreviewPane, L"showPreviewPane"),
                     L"预览窗格：仅新标签生效（显示中的标签不重建）"});

        // ---- 序列键 ----
        v.push_back(
            {L"seqEnabled", Kind::Bool, [&cfg] { return json::Value::MakeBool(cfg.s().seqEnabled); },
             BoolSet(&m.seqEnabled, L"seqEnabled"), L"启用序列键：关闭后只认普通快捷键"});
        v.push_back({L"seqTimeoutMs", Kind::Int,
                     [&cfg] { return json::Value::MakeInt(cfg.s().seqTimeoutMs); },
                     IntSet(&m.seqTimeoutMs, L"seqTimeoutMs", 200, 5000),
                     L"序列键超时(ms)：两键之间超过它就从首键重新开始；太小难按、太大易误触",
                     200, 5000});
        v.push_back({L"seqShowHints", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().seqShowHints); },
                     BoolSet(&m.seqShowHints, L"seqShowHints"), L"显示序列提示浮层"});
        v.push_back({L"seqSingleInstant", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().seqSingleInstant); },
                     BoolSet(&m.seqSingleInstant, L"seqSingleInstant"),
                     L"单键即时：只有一个候选时立即执行，不等第二键"});
        v.push_back({L"seqUniqueAuto", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().seqUniqueAuto); },
                     BoolSet(&m.seqUniqueAuto, L"seqUniqueAuto"),
                     L"唯一候选自动：候选只剩一个时自动执行"});
        v.push_back({L"holdDelayMs", Kind::Int,
                     [&cfg] { return json::Value::MakeInt(cfg.s().holdDelayMs); },
                     IntSet(&m.holdDelayMs, L"holdDelayMs", 200, 2000),
                     L"长按阈值(ms)：按住多久算长按，必须大于双击间隔", 200, 2000});

        // ---- 标签 ----
        v.push_back({L"tabNewPos", Kind::Str, [&cfg] { return json::Value::MakeStr(cfg.s().tabNewPos); },
                     StrEnumSet(&m.tabNewPos, L"tabNewPos", {L"right", L"after_current", L"end"}),
                     L"新标签位置：right 最右 / after_current 当前右侧 / end 末尾"});
        v.push_back({L"confirmCloseTabs", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().confirmCloseTabs); },
                     BoolSet(&m.confirmCloseTabs, L"confirmCloseTabs"),
                     L"关闭窗口且标签多于一个时先确认（最后一个标签关闭不弹）"});
        v.push_back({L"tabMinWidth", Kind::Int,
                     [&cfg] { return json::Value::MakeInt(cfg.s().tabMinWidth); },
                     IntSet(&m.tabMinWidth, L"tabMinWidth", 60, 320),
                     L"标签最小宽度（逻辑像素）：紧凑可设 80", 60, 320});
        v.push_back({L"tabMaxWidth", Kind::Int,
                     [&cfg] { return json::Value::MakeInt(cfg.s().tabMaxWidth); },
                     IntSet(&m.tabMaxWidth, L"tabMaxWidth", 60, 420),
                     L"标签最大宽度（逻辑像素）：长标题在上限处省略", 60, 420});

        // ---- 窗口几何（只读）----
        // 直接改 window.x/y 而不改 window.valid，窗口会在下次启动落到奇怪位置；
        // 而 w/h 只是"上次尺寸"的记录。这类"改了看不见效果"的项不让机器碰。
        //
        // 键名带 `window.` 前缀：settings.json 里几何确实嵌在 window 对象下，
        // 点分路径就是文件里的路径 —— 键名与用户手改文件时看到的完全对应。
        v.push_back({L"window.max", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().maximized); }, nullptr,
                     L"上次是否最大化（只读：下次启动按它恢复，改它当前窗口不变）"});
        v.push_back({L"window.x", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().winX); },
                     nullptr, L"窗口左上角 X（只读：由用户拖拽决定，下次启动生效）"});
        v.push_back({L"window.y", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().winY); },
                     nullptr, L"窗口左上角 Y（只读：由用户拖拽决定，下次启动生效）"});
        v.push_back({L"window.w", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().winW); },
                     nullptr, L"窗口宽（只读：由用户拖拽决定，下次启动生效）"});
        v.push_back({L"window.h", Kind::Int, [&cfg] { return json::Value::MakeInt(cfg.s().winH); },
                     nullptr, L"窗口高（只读：由用户拖拽决定，下次启动生效）"});

        // ---- 会话 / 历史 / 个性化（只读）----
        // 这些都是"运行时另有一份活状态"的数据：写配置文件不会改变已开着的
        // 标签、锁定集、书签树。要改它们请用对应命令（tab.* / bmk.*）。
        v.push_back({L"recentPaths", Kind::StrArray,
                     [&cfg] { return StrArrayValue(cfg.s().recentPaths); }, nullptr,
                     L"最近访问目录（只读：由导航产生，用 history.clear 清空）"});
        v.push_back({L"sessionPaths", Kind::StrArray,
                     [&cfg] { return StrArrayValue(cfg.s().sessionPaths); }, nullptr,
                     L"上次关闭时的标签路径（只读：用 session.save / session.restore）"});
        v.push_back({L"sessionActive", Kind::Int,
                     [&cfg] { return json::Value::MakeInt(cfg.s().sessionActive); }, nullptr,
                     L"上次的活动标签下标（只读：session.save 写入）"});
        v.push_back({L"lockedPaths", Kind::StrArray,
                     [&cfg] { return StrArrayValue(cfg.s().lockedPaths); }, nullptr,
                     L"锁定的标签路径（只读：用 tab.lock / tab.unlock）"});
        v.push_back({L"pinnedPaths", Kind::StrArray,
                     [&cfg] { return StrArrayValue(cfg.s().pinnedPaths); }, nullptr,
                     L"固定的标签路径（只读：用 tab.pin / tab.unpin）"});
        v.push_back({L"namedSessions", Kind::Object,
                     [&cfg] {
                         json::Value a = json::Value::MakeArray();
                         for (const auto& e : cfg.s().namedSessions) {
                             json::Value o = json::Value::MakeObject();
                             o.Set(L"name", json::Value::MakeStr(e.name));
                             json::Value ps = json::Value::MakeArray();
                             for (const auto& p : e.paths) ps.arr.push_back(json::Value::MakeStr(p));
                             o.Set(L"paths", ps);
                             a.arr.push_back(std::move(o));
                         }
                         return a;
                     }, nullptr,
                     L"命名会话（只读：用 session.named.save / .restore / .delete）"});
        v.push_back({L"tabMeta", Kind::Object,
                     [&cfg] { return TabMetaValue(cfg.s().tabMeta); }, nullptr,
                     L"标签改名/颜色（只读：用 tab.rename / tab.color）"});
        v.push_back({L"closedTabs", Kind::StrArray,
                     [&cfg] { return StrArrayValue(cfg.s().closedTabs); }, nullptr,
                     L"关闭历史（只读：用 tab.reopen）"});

        // ---- 状态栏段 ----
        // 段显隐的正确入口是 config.set statusSegs（状态栏右键菜单转投它）。
        // 过去这里只读，理由是"直接写配置不会重建状态栏"——那个理由已经被
        // config.set 的刷新钩子解决（见 app_commands_misc.cpp 的 ConfigSet）。
        // IntArraySet 逐元素校验段号上界，与右键菜单的段号来源同一口径。
        v.push_back({L"statusSegs", Kind::IntArray,
                     [&cfg] { return IntArrayValue(cfg.s().statusSegs); },
                     IntArraySet(&m.statusSegs, L"statusSegs", 0, Seg_Count),
                     L"状态栏显示哪些段（0..Seg_Count-1；右键状态栏勾选）"});

        // ---- 键位 ----
        v.push_back({L"hotkeys", Kind::Object,
                     [&cfg] { return HotkeyValue(cfg.s().hotkeyOverrides); }, nullptr,
                     L"快捷键覆盖（只读：改 data\\keymap.json，或用键位相关命令）"});

        // ---- AI 通道 ----
        // aiEnabled 支持热插拔：config.set 之后命令层会调 App::SyncAiChannel
        // 立即启停管道服务（UI 线程、响应写回之后），无需重启。
        v.push_back({L"aiEnabled", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().aiEnabled); },
                     BoolSet(&m.aiEnabled, L"aiEnabled"),
                     L"启用 AI 通道（立即生效：开启后监听管道，关闭后停止监听）"});
        // aiAllowDanger 每次危险命令都现读，改了立刻生效 —— 这是唯一允许
        // 运行中修改的"安全开关"。
        v.push_back({L"aiAllowDanger", Kind::Bool,
                     [&cfg] { return json::Value::MakeBool(cfg.s().aiAllowDanger); },
                     BoolSet(&m.aiAllowDanger, L"aiAllowDanger"),
                     L"允许危险命令：关闭时删除/覆盖类命令一律拒绝，即使调用方带 approve"});
        return v;
    }();
    return t;
}

// 写后回读用的比较：只比同类型基本值，不做隐式转换
// （1 与 true 相等是最该报出来的错，而不是最该放过的情况）。
bool SameValue(const json::Value& a, const json::Value& b) {
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
            if (!SameValue(a.arr[i], b.arr[i])) return false;
        return true;
    case json::Value::kObject:
        if (a.obj.size() != b.obj.size()) return false;
        for (const auto& kv : a.obj) {
            const json::Value* other = b.Find(kv.first.c_str());
            if (!other || !SameValue(kv.second, *other)) return false;
        }
        return true;
    case json::Value::kNull: return true;
    }
    return false;
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

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Bool: return "bool";
    case Kind::Int: return "int";
    case Kind::Str: return "string";
    case Kind::StrArray: return "string[]";
    case Kind::IntArray: return "int[]";
    case Kind::Object: return "object(readonly)";
    }
    return "?";
}

const std::vector<Entry>& Entries() { return Table(); }

const Entry* Find(const std::wstring& key) {
    for (const auto& e : Entries())
        if (e.key == key) return &e;
    return nullptr;
}

bool Get(const std::wstring& key, json::Value* out) {
    const Entry* e = Find(key);
    if (!e || !out || !e->get) return false;
    *out = e->get();
    return true;
}

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

    // 【写后回读】赋值语句跑过 ≠ 状态真的变了。三种翻车能被这一步抓住：
    //   · 写了 A 字段、读取却读 B 字段（命名漂移）；
    //   · 值被截断/归一（把 5000 存成 500）；
    //   · get 与 set 指向不同副本（两份状态）。
    // 静默成功是设置类 bug 最贵的形态：改了、界面没变，日志却说成功。
    json::Value back;
    if (!Get(key, &back)) {
        if (err) *err = key + L" 写入后读不回来（get 与 set 不在同一份状态上）";
        return false;
    }
    if (!SameValue(back, value)) {
        if (err)
            *err = key + L" 写入后回读不一致：期望 " + json::Stringify(value, 0) + L"，实际 " +
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
        o.Set(L"writable", json::Value::MakeBool(static_cast<bool>(e.set)));
        if (e.get) o.Set(L"value", e.get());
        // 范围只对 Int 有意义；未声明范围时不给 min/max，免得调用方以为
        // "0..0" 是真实取值域。
        if (e.kind == Kind::Int && e.maxValue > e.minValue) {
            o.Set(L"min", json::Value::MakeInt(e.minValue));
            o.Set(L"max", json::Value::MakeInt(e.maxValue));
        }
        items.arr.push_back(std::move(o));
    }
    return items;
}

std::vector<std::wstring> SelfCheck() {
    std::vector<std::wstring> bad;
    for (const auto& e : Entries()) {
        json::Value cur;
        if (!Get(e.key, &cur)) {
            bad.push_back(e.key + L"：读不出来（get 为空或实现有误）");
            continue;
        }
        if (!e.set) continue;   // 只读项只需要能读
        // 写回原值：既不改变状态，又能证明 set 真的接线了。
        std::wstring err;
        if (!Set(e.key, cur, &err)) bad.push_back(e.key + L"：写回原值失败（" + err + L"）");
    }
    return bad;
}

}  // namespace settings_reg
