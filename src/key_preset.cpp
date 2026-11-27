// 见 key_preset.h：预设文件的解析（纯数据层，不碰窗口与配置）。
#include "key_preset.h"

#include <algorithm>

#include "json.h"
#include "log.h"
#include "text_file.h"

namespace keypreset {

namespace {

// 从 JSON 根节点读一个方案。缺 id 或 name 的方案丢弃 —— 没有 id 就没法被
// 命令引用（套用时按 id 找），没有 name 就没法在菜单里显示，两者缺一即不可用。
bool ReadOne(const json::Value& v, Preset* out) {
    if (!v.IsObject() || !out) return false;
    const std::wstring id = v.GetStr(L"id");
    const std::wstring name = v.GetStr(L"name");
    if (id.empty() || name.empty()) return false;
    out->id = id;
    out->name = name;
    out->desc = v.GetStr(L"desc");
    out->hotkeys.clear();
    const json::Value* hk = v.Find(L"hotkeys");
    if (hk && hk->IsObject()) {
        for (const auto& kv : hk->obj) {
            // 值必须是字符串：空串有意义（显式解绑），但非字符串（比如误写成
            // 数字）要跳过 —— 硬塞进去会得到一个"键位是 3"的怪东西。
            if (!kv.second.IsString()) continue;
            out->hotkeys.emplace_back(kv.first, kv.second.str);
        }
    }
    return true;
}

}  // namespace

std::vector<Preset> BuiltinFallback() {
    // 只有"程序默认"一项：等于把功能降级为"一键还原默认"。
    // 这比返回空列表好 —— 空列表会让界面画出一个空菜单，看起来像坏了。
    Preset p;
    p.id = L"default";
    p.name = L"程序默认";
    p.desc = L"当前版本的出厂键位。套用它等于「全部还原默认」。";
    return {p};
}

std::vector<Preset> Load(const std::wstring& path) {
    std::wstring text;
    if (!tf::ReadText(path, &text)) {
        LogDbg(L"[keypreset] 读不到预设文件（%s），退回内置单方案", path.c_str());
        return BuiltinFallback();
    }
    json::Value root;
    std::wstring err;
    if (!json::Parse(text, root, &err) || !root.IsObject()) {
        LogDbg(L"[keypreset] 解析失败（%s）：%s", path.c_str(), err.c_str());
        return BuiltinFallback();
    }
    const json::Value* arr = root.Find(L"presets");
    if (!arr || !arr->IsArray()) {
        LogDbg(L"[keypreset] 缺少 presets 数组（%s）", path.c_str());
        return BuiltinFallback();
    }

    std::vector<Preset> out;
    for (const auto& v : arr->arr) {
        Preset p;
        if (ReadOne(v, &p)) out.push_back(std::move(p));
    }
    // 一个有效方案都没有 = 文件写坏了。退回内置单方案，而不是让菜单空着。
    if (out.empty()) return BuiltinFallback();

    // "程序默认"必须排第一：它是"我什么都不想改"的那个选项，
    // 藏在列表中间会让人找不到"怎么改回去"。
    std::stable_sort(out.begin(), out.end(), [](const Preset& a, const Preset& b) {
        return (a.id == L"default") > (b.id == L"default");
    });
    return out;
}

std::vector<Preset> LoadFromRoot(const std::wstring& root) {
    return Load(tf::Join(tf::Join(root, L"data"), L"keymap_presets.json"));
}

const Preset* Find(const std::vector<Preset>& all, const std::wstring& id) {
    for (const Preset& p : all)
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace keypreset
