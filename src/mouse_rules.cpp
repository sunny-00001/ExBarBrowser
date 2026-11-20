#include "mouse_rules.h"

#include "common.h"
#include "json.h"
#include "text_file.h"

namespace {

struct NameMap {
    const wchar_t* name;
    int value;
};

const NameMap kTargets[] = {
    {L"unknown", MT_UNKNOWN},    {L"empty", MT_EMPTY},
    {L"file", MT_FILE},          {L"folder", MT_FOLDER},
    {L"tab", MT_TAB},            {L"tab_close", MT_TAB_CLOSE},
    {L"new_tab", MT_NEW_TAB},    {L"bookmark", MT_BOOKMARK},
    {L"bookmark_folder", MT_BOOKMARK_FOLDER},
    {L"bookmark_empty", MT_BOOKMARK_EMPTY},
    {L"nav", MT_NAV},            {L"tree", MT_TREE},
    {L"address", MT_ADDRESS},    {L"search", MT_SEARCH},
    {L"toolbar", MT_TOOLBAR},    {L"status", MT_STATUS},
    {L"status_seg", MT_STATUS_SEG},
    {L"view", MT_VIEW},          {L"title", MT_TITLE},
    {L"overflow", MT_OVERFLOW},  {L"group", MT_GROUP},
    {L"column", MT_COLUMN},      {L"preview", MT_PREVIEW},
};

const NameMap kButtons[] = {
    {L"left", MBTN_LEFT}, {L"right", MBTN_RIGHT}, {L"middle", MBTN_MIDDLE},
    {L"x1", MBTN_X1},     {L"x2", MBTN_X2},
};

const NameMap kTriggers[] = {
    {L"single", TR_SINGLE},         {L"double", TR_DOUBLE},
    {L"hold", TR_HOLD},             {L"wheel_up", TR_WHEEL_UP},
    {L"wheel_down", TR_WHEEL_DOWN}, {L"wheel_left", TR_WHEEL_LEFT},
    {L"wheel_right", TR_WHEEL_RIGHT},
};

const NameMap* FindName(const NameMap* table, size_t n, const std::wstring& name) {
    for (size_t i = 0; i < n; ++i)
        if (_wcsicmp(table[i].name, name.c_str()) == 0) return &table[i];
    return nullptr;
}

}  // namespace

const wchar_t* MouseTargetName(int target) {
    for (const auto& e : kTargets)
        if (e.value == target) return e.name;
    return L"unknown";
}

const wchar_t* MouseButtonName(int button) {
    for (const auto& e : kButtons)
        if (e.value == button) return e.name;
    return L"left";
}

const wchar_t* MouseTriggerName(int trigger) {
    for (const auto& e : kTriggers)
        if (e.value == trigger) return e.name;
    return L"single";
}

int MouseTargetFromName(const std::wstring& name) {
    const NameMap* e = FindName(kTargets, _countof(kTargets), name);
    return e ? e->value : -1;
}

int MouseButtonFromName(const std::wstring& name) {
    const NameMap* e = FindName(kButtons, _countof(kButtons), name);
    return e ? e->value : -1;
}

int MouseTriggerFromName(const std::wstring& name) {
    const NameMap* e = FindName(kTriggers, _countof(kTriggers), name);
    return e ? e->value : -1;
}

MouseRules& MouseRules::Get() {
    static MouseRules r;
    return r;
}

void MouseRules::LoadDefaults() {
    rules_.clear();
    // 与 Exbar.cpp 一致：双击文件列表空白 -> 上一级目录。
    // 用户配置中的同组合规则放在后面，Find 从后往前找，用户规则优先。
    MouseRule up;
    up.target = MT_EMPTY;
    up.button = MBTN_LEFT;
    up.trigger = TR_DOUBLE;
    up.mods = 0;
    up.action = L"command";
    up.command = L"NavUp";
    up.builtin = true;
    rules_.push_back(up);

    // 与 mod 一致的内置默认规则：双击书签栏空白 -> 切换书签栏。
    // 用户无需配置即生效；即使配置文件里也写了同一条，多规则并存（后写的优先）。
    MouseRule r;
    r.target = MT_BOOKMARK_EMPTY;
    r.button = MBTN_LEFT;
    r.trigger = TR_DOUBLE;
    r.mods = 0;
    r.action = L"command";
    r.command = L"BmkToggleBar";
    r.builtin = true;
    rules_.push_back(r);

    // 状态栏空白双击：复制当前目录（与 mod 的"状态栏段可点击"习惯一致）
    MouseRule s;
    s.target = MT_STATUS;
    s.button = MBTN_LEFT;
    s.trigger = TR_DOUBLE;
    s.mods = 0;
    s.action = L"command";
    s.command = L"PathCopyDir";
    s.builtin = true;
    rules_.push_back(s);
}

void MouseRules::Load(const std::wstring& path) {
    LoadDefaults();

    std::wstring text;
    if (!tf::ReadText(path, &text)) return;
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err)) {
        LogDbg(L"[mouse] 解析失败（%s）：%s", path.c_str(), err.c_str());
        return;
    }
    const json::Value* list = root.IsArray() ? &root : root.Find(L"rules");
    if (!list || !list->IsArray()) return;

    std::vector<MouseRule> user;
    for (const auto& v : list->arr) {
        if (!v.IsObject()) continue;
        MouseRule r;
        r.target = MouseTargetFromName(v.GetStr(L"target", L"empty"));
        r.button = MouseButtonFromName(v.GetStr(L"button", L"left"));
        r.trigger = MouseTriggerFromName(v.GetStr(L"trigger", L"single"));
        r.mods = v.GetInt(L"mods", 0);
        r.action = v.GetStr(L"action", L"none");
        r.command = v.GetStr(L"command");
        r.hotkey = v.GetStr(L"hotkey");
        if (r.target < 0 || r.button < 0 || r.trigger < 0) {
            LogDbg(L"[mouse] 跳过非法规则 target=%s", v.GetStr(L"target").c_str());
            continue;
        }
        if (r.action == L"command" && r.command.empty()) {
            LogDbg(L"[mouse] 规则 action=command 但缺少 command 字段，已跳过");
            continue;
        }
        user.push_back(std::move(r));
    }
    // 用户规则放在内置规则之后：Find 从后往前找，用户配置优先
    rules_.insert(rules_.end(), user.begin(), user.end());
}

bool MouseRules::HasPointerRules(int button) const {
    for (const auto& r : rules_) {
        if (r.button != button) continue;
        if (r.trigger == TR_SINGLE || r.trigger == TR_DOUBLE || r.trigger == TR_HOLD) return true;
    }
    return false;
}

bool MouseRules::HasWheelRules() const {
    for (const auto& r : rules_) {
        if (r.trigger == TR_WHEEL_UP || r.trigger == TR_WHEEL_DOWN ||
            r.trigger == TR_WHEEL_LEFT || r.trigger == TR_WHEEL_RIGHT)
            return true;
    }
    return false;
}

bool MouseRules::HasTriggerRules(int button, int trigger) const {
    for (const auto& r : rules_)
        if (r.button == button && r.trigger == trigger) return true;
    return false;
}

int CurrentMods() {
    int m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 1;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
    // AltGr = 右 Alt + 左 Ctrl：字符输入，不是修饰键组合
    if ((GetKeyState(VK_RMENU) & 0x8000) && (GetKeyState(VK_LCONTROL) & 0x8000)) return 0;
    return m;
}

const MouseRule* MouseRules::Find(int target, int button, int trigger, int mods) const {
    for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
        if (it->target == target && it->button == button && it->trigger == trigger &&
            it->mods == mods)
            return &*it;
    }
    return nullptr;
}
