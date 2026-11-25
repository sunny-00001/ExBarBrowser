// 文件属性写入的唯一实现（file.attr.set / fs.attr.set 共用）。
//
// 只改真正传了的属性位：没传的位保持原值。用 set/clear 两个掩码表达
// "要置 1 / 要清 0"，避免用 bool 默认值把"没传"误当成"要关掉"。
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "json.h"

namespace fileattr {

struct Delta {
    DWORD set = 0;    // 要置 1 的 FILE_ATTRIBUTE_* 位
    DWORD clear = 0;  // 要清 0 的 FILE_ATTRIBUTE_* 位
    bool Empty() const { return set == 0 && clear == 0; }
};

constexpr DWORD kWritableBits = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;

// 从 {"readonly":true,...} 解析出 set/clear 掩码。
// 未出现的字段不改动；出现了但不是布尔值 → 整批拒绝（不猜意图）。
bool ParseDelta(const json::Value& attrs, Delta* out);

// 整批语义：先全部预检，任一条不可写就一条都不改（避免"改了一半"）。
// 写后逐条回读校验；out 回报每条路径写完后的属性位（顺序同 paths）。
bool Apply(const std::vector<std::wstring>& paths, const Delta& d,
           std::vector<DWORD>* out);

// 两个 handler 共用的结果打包（属性位不在 state 快照里，只能由命令回报）。
inline void WriteResult(const std::vector<DWORD>& bits, json::Value* out) {
    if (!out) return;
    json::Value arr = json::Value::MakeArray();
    for (DWORD b : bits) arr.arr.push_back(json::Value::MakeInt(static_cast<int>(b)));
    out->Set(L"attrs", std::move(arr));
    out->Set(L"count", json::Value::MakeInt(static_cast<int>(bits.size())));
}

}  // namespace fileattr
