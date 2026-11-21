// 设置面板：由设置项键表（settings_reg）驱动的界面。
//
// 【为什么不是一堆手写复选框】
// 每一行都来自 settings_reg::Entries()，改动只发生在表里：加一行就多一个可调项，
// 界面 / 命令 / AI 三边同时看到它。手写控件的话，"表里有、界面没有"会变成
// 静默的能力缺失 —— 而那种缺失没有任何检查会报。
//
// 【人机同源】
// 面板不直接改 Settings 字段，而是调 config.set 命令 —— 与 AI / 脚本走同一条路。
// 于是面板上的每次改动都有审计记录、都过 Set 的类型校验，不会出现
// "界面能改、命令改不动"或"命令改了、界面不知道"。
//
// 【行文案】"中文名：当前值  (键名)"。带上键名是有意的：用户看到 tab=开，
// 就能直接对 AI 说"把 tab 关掉"，两边说的是同一个名字。
//
// 【交互】三种形态，键盘全程可用：
//   · 布尔：点一下即翻转 —— 它可逆、立即生效，加二次确认只会拖慢调参；
//   · 数值/字符串：弹输入框，初值是当前值。类型错由注册表拒绝并给出原因；
//   · 数组：输入框填逗号分隔，空串 = 空数组。
// 每次改动后面板重新打开并停在原项：连调多项不必重新找位置，且立刻看到新值。
// 失败一律弹窗说明原因 —— "点了没反应"在设置里是最贵的体验。
#include "app.h"

#include <cwchar>

#include "menu.h"
#include "prompt.h"
#include "settings_reg.h"
#include "str.h"

namespace {

// 值的显示形态：只表达类型与规模，不在这里做"美化"。
// 例如 viewMode 的 3 不翻成"中图标" —— 那需要第二张 下标→名字 的表，
// 两张表迟早分叉，而分叉点只有用户能发现。
std::wstring ValueText(settings_reg::Kind k, const json::Value& v) {
    switch (k) {
    case settings_reg::Kind::Bool: return v.b ? L"开" : L"关";
    case settings_reg::Kind::Int: return std::to_wstring(static_cast<long long>(v.num));
    case settings_reg::Kind::Str: return v.str;
    case settings_reg::Kind::StrArray:
        return std::to_wstring(static_cast<long long>(v.arr.size())) + L" 项";
    case settings_reg::Kind::IntArray:
        return std::to_wstring(static_cast<long long>(v.arr.size())) + L" 项";
    case settings_reg::Kind::Object:
        // 复合结构（tabMeta / hotkeys）展开成一行没有意义，界面也不该编辑它。
        return std::wstring(L"（复合结构）");
    }
    return std::wstring();
}

// 输入框的初值：数组按逗号拼回去，其余直接取文本。
std::wstring EditText(settings_reg::Kind k, const json::Value& v) {
    switch (k) {
    case settings_reg::Kind::Bool: return v.b ? L"true" : L"false";
    case settings_reg::Kind::Int: return std::to_wstring(static_cast<long long>(v.num));
    case settings_reg::Kind::Str: return v.str;
    case settings_reg::Kind::StrArray:
    case settings_reg::Kind::IntArray: {
        std::wstring s;
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i) s += L",";
            const json::Value& e = v.arr[i];
            s += e.IsString() ? e.str : std::to_wstring(static_cast<long long>(e.num));
        }
        return s;
    }
    case settings_reg::Kind::Object: return std::wstring();
    }
    return std::wstring();
}

// 逗号/分号分隔的列表 → 数组元素。空串 = 空数组（"清空"是合法意图）。
std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L'，' || c == L'；') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != L' ' && c != L'\t') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 写一项：走命令总线，不直接碰 Settings。
// 失败原因取自总线的 lastBlockReason（handler 用 NoteFailure 报上来的那段），
// 面板只负责显示，不自己判断"为什么不行"。
bool WriteSetting(const std::wstring& key, const json::Value& value, std::wstring* reason) {
    json::Value args = json::Value::MakeObject();
    args.Set(L"key", json::Value::MakeStr(key));
    args.Set(L"value", value);  // Set 收 by-value：这里传的是副本
    if (CommandBus::Get().Invoke(CmdId::ConfigSet, Args::FromJson(args), Actor::Settings()))
        return true;
    if (reason) *reason = CommandBus::Get().lastBlockReason();
    return false;
}

// 读一项：同样走命令（config.get）。
//
// 行文案里的值是渲染时直读表的（一屏几十行，不能每行发一次命令），
// 但**编辑前**要读一次最新的：从上次重绘到现在，值可能已经被命令/AI 改过，
// 拿旧值去覆盖等于把别人的改动吞掉。这一读也让"人要改某项"和"AI 要读某项"
// 走同一条路（同一条审计、同一份校验）。
bool ReadSetting(const std::wstring& key, json::Value* out, std::wstring* reason) {
    json::Value args = json::Value::MakeObject();
    args.Set(L"key", json::Value::MakeStr(key));
    json::Value result;
    if (!CommandBus::Get().InvokeByNameResult(L"config.get", Args::FromJson(args), &result, Actor::Settings())) {
        if (reason) *reason = CommandBus::Get().lastBlockReason();
        return false;
    }
    const json::Value* v = result.Find(L"value");
    if (!v) {
        if (reason) *reason = key + L"：config.get 没返回 value";
        return false;
    }
    *out = *v;
    return true;
}

}  // namespace

void App::ShowSettingsPanel(POINT at) {
    const std::vector<settings_reg::Entry>& entries = settings_reg::Entries();

    // 改完一项后重新打开面板，并把光标停回那一项：连着调三项不必重新找位置。
    int focus = -1;

    for (;;) {
        ui::Menu menu;
        int initialRow = -1;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const settings_reg::Entry& e = entries[static_cast<size_t>(i)];
            const json::Value cur = e.get ? e.get() : json::Value();
            std::wstring label = e.doc.empty() ? e.key : e.doc;
            label += L"：" + ValueText(e.kind, cur);
            const bool on = e.kind == settings_reg::Kind::Bool && cur.b;
            // 只读项灰显：能看见有哪些项、当前值是什么，但点不动 ——
            // 比"干脆不列出来"诚实（用户不会以为程序没有这个设置）。
            const int row = menu.AddChecked(label, on, i, ico::IconId::None, !e.set);
            // 键名追加在行尾：人要照这个名字去说/去写脚本，机器也用同一个名字。
            menu.AppendLabel(row, L"  (" + e.key + L")");
            if (i == focus) initialRow = row;
        }

        ui::MenuOptions opt;
        opt.initialRow = initialRow;
        // 全部项一屏可见：设置面板出滚动条会让"总共有哪些项"变得不直观。
        opt.maxRows = static_cast<int>(entries.size()) + 4;
        const int action = menu.Show(hwnd_, theme_, at, opt);
        if (action < 0) return;   // Esc / 点外面 = 关面板

        focus = action;
        const settings_reg::Entry& e = entries[static_cast<size_t>(action)];
        if (!e.set) {
            // 灰显行理论上点不到，这里兜一层：能点到就必须有反馈。
            MessageBeep(MB_ICONWARNING);
            continue;
        }

        // 布尔项直接翻转：这类项的意义就是"开/关"，再弹一层只会多一次点击。
        if (e.kind == settings_reg::Kind::Bool) {
            json::Value cur;
            std::wstring reason;
            if (!ReadSetting(e.key, &cur, &reason) || !WriteSetting(e.key, json::Value::MakeBool(!cur.b), &reason)) {
                const std::wstring why = reason.empty() ? L"（命令层没有给出原因）" : reason;
                MessageBoxW(hwnd_, (e.key + L" 没有改成：\n\n" + why).c_str(), L"设置未生效",
                            MB_OK | MB_ICONWARNING);
            }
            continue;
        }

        // 其余类型都靠输入框：值域/枚举的权威在注册表，界面不另存一份规则。
        json::Value cur;
        std::wstring readErr;
        if (!ReadSetting(e.key, &cur, &readErr)) {
            MessageBoxW(hwnd_, (readErr.empty() ? e.key + L" 读不出来" : readErr).c_str(),
                        L"设置读不到", MB_OK | MB_ICONWARNING);
            continue;
        }
        std::wstring text = EditText(e.kind, cur);
        const std::wstring label = (e.doc.empty() ? e.key : e.doc) + L"（" + e.key + L"  " +
                                   str::FromUtf8(settings_reg::KindName(e.kind)) + L"）";
        if (!PromptText(hwnd_, theme_, L"修改设置", label, text, &text))
            return;   // 取消输入 = 收面板（用户明确表示不想改了）

        json::Value next;
        bool ready = true;
        switch (e.kind) {
        case settings_reg::Kind::Int: {
            wchar_t* end = nullptr;
            const long v = std::wcstol(text.c_str(), &end, 10);
            while (end && (*end == L' ' || *end == L'\t')) ++end;
            if (text.empty() || !end || *end != L'\0') {
                MessageBoxW(hwnd_, (L"「" + text + L"」不是整数。").c_str(), L"设置未改",
                            MB_OK | MB_ICONWARNING);
                ready = false;
            } else {
                next = json::Value::MakeInt(static_cast<int>(v));
            }
            break;
        }
        case settings_reg::Kind::Str:
            next = json::Value::MakeStr(text);
            break;
        case settings_reg::Kind::StrArray: {
            next = json::Value::MakeArray();
            for (const auto& one : SplitList(text)) next.arr.push_back(json::Value::MakeStr(one));
            break;
        }
        case settings_reg::Kind::IntArray: {
            next = json::Value::MakeArray();
            for (const auto& one : SplitList(text)) {
                wchar_t* end = nullptr;
                const long v = std::wcstol(one.c_str(), &end, 10);
                if (one.empty() || !end || *end != L'\0') {
                    MessageBoxW(hwnd_, (L"「" + one + L"」不是整数。").c_str(), L"设置未改",
                                MB_OK | MB_ICONWARNING);
                    ready = false;
                    break;
                }
                next.arr.push_back(json::Value::MakeInt(static_cast<int>(v)));
            }
            break;
        }
        default:
            ready = false;   // Bool 已在上面处理；Object 不可写
            break;
        }
        if (!ready) continue;

        std::wstring reason;
        if (!WriteSetting(e.key, next, &reason)) {
            const std::wstring why = reason.empty() ? L"（命令层没有给出原因）" : reason;
            MessageBoxW(hwnd_, (e.key + L" 没有改成：\n\n" + why).c_str(), L"设置未生效",
                        MB_OK | MB_ICONWARNING);
        }
    }
}
