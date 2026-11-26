// 设置键表的**只读观测面**：键位 / 窗口几何 / 会话状态。
//
// 这三组的 set 一律是 nullptr —— config.set 对它们一律拒绝，理由是同一个：
// 配置文件里的这一份**不是**活状态。改它不会改已开着的标签、锁定集、窗口位置，
// 只会让"下次启动"与"当前所见"分叉。所以它们的 doc 都以"只读：用 xxx 命令"结尾。
//
// 表本体的其余部分（可写的偏好设置）在 settings_reg_table.cpp；分段理由见
// settings_reg_table_readonly.h。
#include "settings_reg_table_readonly.h"

#include "config.h"
#include "settings_reg_valid.h"   // HotkeyValue / TabMetaValue / TabHistoryValue / StrArrayValue

namespace settings_reg {
namespace detail {

void AppendReadonlyEntries(std::vector<Entry>& v) {
    // ---- 键位（只读）：写入走 hotkey.set / hotkey.clear / hotkey.reset ----
    v.push_back({L"hotkeys", Kind::Object,
                 [](const Settings& s) { return HotkeyValue(s.hotkeyOverrides); }, nullptr,
                 L"快捷键覆盖（只读：改 data\\keymap.json，或用 hotkey.set 等三条命令）", L"keys"});

    // ---- 窗口几何（只读）----
    // 直接改 window.x/y 而不改 window.valid，窗口会在下次启动落到奇怪位置；
    // 而 w/h 只是"上次尺寸"的记录。这类"改了看不见效果"的项不让机器碰。
    //
    // 键名带 `window.` 前缀：settings.json 里几何确实嵌在 window 对象下，
    // 点分路径就是文件里的路径 —— 键名与用户手改文件时看到的完全对应。
    v.push_back({L"window.max", Kind::Bool,
                 [](const Settings& s) { return json::Value::MakeBool(s.maximized); }, nullptr,
                 L"上次是否最大化（只读：下次启动按它恢复，改它当前窗口不变）", L"geometry"});
    v.push_back({L"window.x", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.winX); }, nullptr,
                 L"窗口左上角 X（只读：由用户拖拽决定，下次启动生效）", L"geometry"});
    v.push_back({L"window.y", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.winY); }, nullptr,
                 L"窗口左上角 Y（只读：由用户拖拽决定，下次启动生效）", L"geometry"});
    v.push_back({L"window.w", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.winW); }, nullptr,
                 L"窗口宽（只读：由用户拖拽决定，下次启动生效）", L"geometry"});
    v.push_back({L"window.h", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.winH); }, nullptr,
                 L"窗口高（只读：由用户拖拽决定，下次启动生效）", L"geometry"});

    // ---- 会话状态（只读）----
    // 这些都是"运行时另有一份活状态"的数据：写配置文件不会改变已开着的
    // 标签、锁定集、书签树。要改它们请用对应命令（tab.* / bmk.*）。
    v.push_back({L"recentPaths", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.recentPaths); }, nullptr,
                 L"最近访问目录（只读：由导航产生，用 nav.history.clear 清空）", L"session"});
    v.push_back({L"sessionPaths", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.sessionPaths); }, nullptr,
                 L"上次关闭时的标签路径（只读：用 session.save / session.restore）", L"session"});
    v.push_back({L"sessionActive", Kind::Int,
                 [](const Settings& s) { return json::Value::MakeInt(s.sessionActive); }, nullptr,
                 L"上次的活动标签下标（只读：session.save 写入）", L"session"});
    v.push_back({L"lockedPaths", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.lockedPaths); }, nullptr,
                 L"锁定的标签路径（只读：用 tab.lock locked=true/false 锁或解锁）", L"session"});
    v.push_back({L"pinnedPaths", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.pinnedPaths); }, nullptr,
                 L"固定的标签路径（只读：用 tab.pin pinned=true/false 固定或取消）", L"session"});
    v.push_back({L"namedSessions", Kind::ObjArray,
                 [](const Settings& s) {
                     json::Value a = json::Value::MakeArray();
                     for (const auto& e : s.namedSessions) {
                         json::Value o = json::Value::MakeObject();
                         o.Set(L"name", json::Value::MakeStr(e.name));
                         json::Value ps = json::Value::MakeArray();
                         for (const auto& p : e.paths) ps.arr.push_back(json::Value::MakeStr(p));
                         o.Set(L"paths", ps);
                         a.arr.push_back(std::move(o));
                     }
                     return a;
                 }, nullptr,
                 L"命名会话（只读：用 session.named.save / .restore / .delete）", L"session"});
    v.push_back({L"tabMeta", Kind::ObjArray,
                 [](const Settings& s) { return TabMetaValue(s.tabMeta); }, nullptr,
                 L"标签改名/颜色（只读：用 tab.rename / tab.color）", L"session"});
    v.push_back({L"closedTabs", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.closedTabs); }, nullptr,
                 L"关闭历史（只读：用 tab.reopen）", L"session"});
    // 会话的另外三段此前完全没登记：它们在 settings.json 里真实存在
    // （sessionSelection / sessionHistory / sessionPane2Paths），但 config.list
    // 与设置面板都看不见 —— 于是"为什么重启后没选中上次的文件"这类问题
    // 连现场都调不出来。只读的理由与其它会话项一致：改文件不会改活状态。
    v.push_back({L"sessionSelection", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.sessionSelection); }, nullptr,
                 L"上次关闭时活动标签的选中项（只读：由关闭流程写入，重启时尽力恢复）",
                 L"session"});
    v.push_back({L"sessionHistory", Kind::ObjArray,
                 [](const Settings& s) { return TabHistoryValue(s.sessionHistory); }, nullptr,
                 L"每标签的导航历史（只读：session.save 写入；重启时按路径回填）", L"session"});
    v.push_back({L"sessionPane2Paths", Kind::StrArray,
                 [](const Settings& s) { return StrArrayValue(s.sessionPane2Paths); }, nullptr,
                 L"双栏右窗格的路径（只读：与 sessionPaths 下标对齐，空串=当时无右窗格）",
                 L"session"});
    v.push_back({L"sessionPane2History", Kind::ObjArray,
                 [](const Settings& s) { return TabHistoryValue(s.sessionPane2History); },
                 nullptr, L"右窗格的导航历史（只读：只记录有右窗格的那些标签）", L"session"});
}

}  // namespace detail
}  // namespace settings_reg
