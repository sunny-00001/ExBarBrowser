// 输入端的配置边界（R15：input 不得直接依赖 platform/config）。
//
// 【为什么是接口而不是直接读 Config::Get()】键位覆盖、序列开关、鼠标路径这些
// 配置以前由 InputRouter / overlay 直接向 Config 单例读写 —— 读还好，写
// （SetKeyForCommand / ResetDefaults 里的 SetHotkeyOverride + Save + clear）
// 让输入端改了业务状态，与"输入端只发命令、不改状态"冲突。现在每个读点改成
// 细粒度 getter（行为与原 Config::Get().s().X 完全等价，仍是实时读），写点改成
// 回调；本文件不 include config.h，App 层实现接口并转发到 Config / 配置落盘。
// 于是 input 层只依赖 contract 级抽象，方向单向。
#pragma once
#include <string>
#include <utility>
#include <vector>

struct InputConfig {
    virtual ~InputConfig() = default;

    // ---- 读：与原 Config::Get().s().X / Config::Get().Path() 逐项对应 ----
    virtual std::wstring KeymapPath() const = 0;
    virtual std::wstring MousePath() const = 0;
    virtual bool SeqUniqueAuto() const = 0;
    virtual bool SeqSingleInstant() const = 0;
    virtual bool SeqEnabled() const = 0;
    virtual bool SeqShowHints() const = 0;
    virtual int SeqTimeoutMs() const = 0;
    virtual int HoldDelayMs() const = 0;
    virtual const std::vector<std::pair<std::wstring, std::wstring>>& HotkeyOverrides() const = 0;

    // ---- 写：由 App 转发到 Config（配置落盘的唯一路径）----
    virtual void SetHotkeyOverride(const std::wstring& setting, const std::wstring& key) = 0;
    // 删掉一条覆盖项（"恢复此条为默认"）。与 SetHotkeyOverride(setting, L"") 的区别
    // 见 InputRouter::ResetKeyForCommand —— 那个是"绑成空键"，这个是"没有覆盖"。
    virtual void ClearHotkeyOverride(const std::wstring& setting) = 0;
    virtual void ClearAllHotkeyOverrides() = 0;
    virtual void Save() = 0;
};
