#include "keymap.h"

#include "common.h"
#include "json.h"
#include "keymap.gen.h"
#include "text_file.h"

#include <cwctype>

namespace {

// 默认表缓存：首次 DefaultKeymap() 时由生成头填满，之后只读。
// 放在 BuildDefault 之前——它引用这两个对象。
Keymap g_default;
bool g_defaultReady = false;

void BuildDefault() {
    if (g_defaultReady) return;
    for (const auto& d : keymap_gen::kHotkeys)
        g_default.hotkeys.push_back({d.setting, d.command, d.key});
    for (const auto& d : keymap_gen::kSeqs)
        g_default.sequences.push_back({d.seq, SplitSequence(d.seq), d.command});
    g_defaultReady = true;
}

// 声明"这个 setting 的系统默认键"，给 keymap.gen.h 里 key 为空的条目兜底。
// 这里**不放未实现命令的键**：原先有一行 {L"hotkeyBatchRename", L"F2"}，已删。
// 它现在是死数据（hotkeyBatchRename 的 key 是 Ctrl+Shift+R，非空就不查这张表），
// 但留着是个陷阱：只要 keymap 里那条 key 被清空 —— 用户自己的 keymap.json 就可能是
// 空 key，BuildRows/SystemKeyFor 还用它显示键名 —— F2 就会挂到这个未实现命令名下；
// 而 BuildBindings 的 hotkeys 循环跑在 kExtra 之前、add() 又是"先到先得"，等
// BatchRename 将来实现，F2 就会从 CmdRename 手里被抢走，重命名键静默失效。
const struct {
    const wchar_t* setting;
    const wchar_t* combo;
} kSysKeys[] = {
    {L"hotkeyCmdCopy", L"Ctrl+C"},          {L"hotkeyCmdCut", L"Ctrl+X"},
    {L"hotkeyCmdPaste", L"Ctrl+V"},         {L"hotkeyCmdDelete", L"Delete"},
    {L"hotkeyCmdUndo", L"Ctrl+Z"},          {L"hotkeyCmdSelectAll", L"Ctrl+A"},
    {L"hotkeyCmdNewFolder", L"Ctrl+Shift+N"},{L"hotkeyNavBack", L"Alt+←"},
    {L"hotkeyNavForward", L"Alt+→"},        {L"hotkeyNavUp", L"Alt+↑"},
    {L"hotkeyNavRefresh", L"F5"},           {L"hotkeyTogglePreviewPane", L"Alt+P"},
    {L"hotkeyCopyPath", L"Ctrl+Shift+C"},
    {L"hotkeyTabNew", L"Ctrl+T"},           {L"hotkeyTabClose", L"Ctrl+W"},
    {L"hotkeyTabNext", L"Ctrl+Tab"},        {L"hotkeyTabPrev", L"Ctrl+Shift+Tab"},
    {L"hotkeyNewWindow", L"Ctrl+N"},        {L"hotkeyReopenClosedTab", L"Ctrl+Shift+T"},
};

}  // namespace

std::vector<std::wstring> SplitSequence(const std::wstring& seq) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : seq) {
        if (iswspace(c)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(static_cast<wchar_t>(towlower(c)));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const Keymap& DefaultKeymap() {
    BuildDefault();
    return g_default;
}

std::wstring SystemKeyFor(const std::wstring& setting) {
    for (const auto& e : kSysKeys)
        if (_wcsicmp(e.setting, setting.c_str()) == 0) return e.combo;
    return std::wstring();
}

bool LoadKeymap(const std::wstring& path, Keymap* out) {
    if (!out) return false;
    *out = DefaultKeymap();

    std::wstring text;
    if (!tf::ReadText(path, &text)) return false;
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err) || !root.IsObject()) {
        LogDbg(L"[keymap] 解析失败（%s）：%s", path.c_str(), err.c_str());
        return false;
    }

    Keymap km;
    if (const json::Value* hks = root.Find(L"hotkeys"); hks && hks->IsArray()) {
        for (const auto& v : hks->arr) {
            if (!v.IsObject()) continue;
            HotkeyBinding b;
            b.setting = v.GetStr(L"setting");
            b.command = v.GetStr(L"command");
            b.key = v.GetStr(L"key");
            if (!b.setting.empty() && !b.command.empty()) km.hotkeys.push_back(std::move(b));
        }
    }
    if (const json::Value* seqs = root.Find(L"sequences"); seqs && seqs->IsArray()) {
        for (const auto& v : seqs->arr) {
            if (!v.IsObject()) continue;
            SequenceBinding b;
            b.seq = v.GetStr(L"seq");
            b.command = v.GetStr(L"command");
            b.tokens = SplitSequence(b.seq);
            if (b.tokens.size() >= 1 && !b.command.empty()) km.sequences.push_back(std::move(b));
        }
    }
    if (km.hotkeys.empty() && km.sequences.empty()) return false;
    *out = std::move(km);
    return true;
}
