// config_io.cpp 与 config_state_io.cpp 共用的 JSON 读写入口。
//
// 【为什么单独成头】settings.json（偏好）与 state\*.json（状态）用的是同一套
// 数组/对象编解码口径：空串保位、坏项跳过、颜色夹取、历史 idx 夹取。
// 两套文件各写一份 helper 必然漂移（历史上 sessionPane2Paths 就因为
// 写盘保留空串、读盘丢空串而整体错位），所以唯一实现放这里，两边都 include。
#pragma once
#include "config.h"

#include <algorithm>

namespace configio {

inline int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// 字符串数组：丢掉空串与异类项（用于"一批路径/词"，空串没有语义）。
inline std::vector<std::wstring> ReadStringArray(const json::Value& root, const wchar_t* key) {
    std::vector<std::wstring> out;
    const json::Value* a = root.Find(key);
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr)
        if (v.IsString() && !v.str.empty()) out.push_back(v.str);
    return out;
}

// 保位读取：空串**保留**，只丢"不是字符串"的项。
// sessionPane2Paths 与 sessionPaths 按下标对齐，空串 = "该标签当时没有右窗格"，
// 是占位符。用会丢空串的读法会让后面的标签整体前移一格。
inline std::vector<std::wstring> ReadStringArrayKeepEmpty(const json::Value& root,
                                                          const wchar_t* key) {
    std::vector<std::wstring> out;
    const json::Value* a = root.Find(key);
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr)
        if (v.IsString()) out.push_back(v.str);
    return out;
}

inline json::Value MakeStringArray(const std::vector<std::wstring>& items) {
    json::Value a = json::Value::MakeArray();
    for (const auto& s : items) a.arr.push_back(json::Value::MakeStr(s));
    return a;
}

inline json::Value MakeIntArray(const std::vector<int>& items) {
    json::Value a = json::Value::MakeArray();
    for (int n : items) a.arr.push_back(json::Value::MakeInt(n));
    return a;
}

inline json::Value MakeTabMeta(const std::vector<TabMeta>& items) {
    json::Value a = json::Value::MakeArray();
    for (const auto& m : items) {
        if (m.path.empty()) continue;
        if (m.alias.empty() && m.color == 0) continue;
        json::Value o = json::Value::MakeObject();
        o.Set(L"path", json::Value::MakeStr(m.path));
        if (!m.alias.empty()) o.Set(L"alias", json::Value::MakeStr(m.alias));
        if (m.color != 0) o.Set(L"color", json::Value::MakeInt(m.color));
        a.arr.push_back(std::move(o));
    }
    return a;
}

inline std::vector<TabMeta> ReadTabMeta(const json::Value& root) {
    std::vector<TabMeta> out;
    const json::Value* a = root.Find(L"tabMeta");
    if (!a || !a->IsArray()) return out;
    for (const auto& v : a->arr) {
        if (!v.IsObject()) continue;
        TabMeta m;
        m.path = v.GetStr(L"path");
        m.alias = v.GetStr(L"alias");
        m.color = v.GetInt(L"color", 0);
        if (m.color < 0 || m.color >= core::kTabColorCount) m.color = 0;
        if (m.path.empty()) continue;
        out.push_back(std::move(m));
    }
    return out;
}

inline std::vector<TabHistory> ReadTabHistories(const json::Value& root, const wchar_t* key) {
    std::vector<TabHistory> out;
    const json::Value* hist = root.Find(key);
    if (!hist || !hist->IsArray()) return out;
    for (const auto& e : hist->arr) {
        if (!e.IsObject()) continue;
        TabHistory h;
        if (const json::Value* ps = e.Find(L"paths"); ps && ps->IsArray())
            for (const auto& p : ps->arr)
                if (p.IsString()) h.paths.push_back(p.str);
        h.idx = e.GetInt(L"idx", 0);
        if (h.idx < 0 || h.idx >= static_cast<int>(h.paths.size())) h.idx = 0;
        out.push_back(std::move(h));
    }
    return out;
}

inline json::Value MakeTabHistories(const std::vector<TabHistory>& list) {
    json::Value a = json::Value::MakeArray();
    for (const auto& h : list) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"paths", MakeStringArray(h.paths));
        o.Set(L"idx", json::Value::MakeInt(h.idx));
        a.arr.push_back(std::move(o));
    }
    return a;
}

}  // namespace configio
