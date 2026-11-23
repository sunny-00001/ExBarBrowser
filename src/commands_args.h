#pragma once
#include <cwchar>
#include <string>
#include <vector>

#include "json.h"

// 命令的风险等级（与 mod 的 risk / 架构规则 R18 对齐）。
// AI 侧据此决定"直接执行 / 必须人工批准 / 一律拒绝"。
enum class Risk {
    Read,     // 只读：不改磁盘、不改视图/选区/焦点
    Write,    // 改状态：切标签、导航、排序、打开面板（可回退，但有副作用）
    Danger,   // 破坏性：删除、覆盖、批量改名（不可轻易回退）
};

// ---------------------------------------------------------------- 参数对象
//
// 命令参数从"一个需要自己拼的字符串"改成"具名对象"（业界共识 P2）。
// 理由不是好看，而是把四件事一次性解决：
//   · 多参数 / 可选参数（字符串做不到）
//   · 类型校验（字符串只能靠 handler 里 _wtoi 猜）
//   · 自动生成 UI 与 MCP inputSchema
//   · 向后兼容地加参数
//
// 内部用 json::Value 存，避免为每种命令定义结构体（127 条命令 → 127 个 struct
// 是过度设计）。取用走具名访问器，拼错键名得到默认值而不是崩溃。
class Args {
public:
    Args() = default;

    static Args FromJson(const json::Value& v) {
        Args a;
        a.v_ = v;
        return a;
    }
    // 空对象：无参数命令、以及"没给参数"的情形。
    static Args Empty() { return Args(); }

    bool IsEmpty() const { return !v_.IsObject() || v_.obj.empty(); }
    const json::Value& raw() const { return v_; }

    bool Has(const wchar_t* key) const { return v_.Find(key) != nullptr; }

    std::wstring Str(const wchar_t* key, const std::wstring& def = std::wstring()) const {
        return Has(key) ? v_.GetStr(key, def) : def;
    }
    int Int(const wchar_t* key, int def = 0) const {
        return Has(key) ? v_.GetInt(key, def) : def;
    }
    bool Bool(const wchar_t* key, bool def = false) const {
        return Has(key) ? v_.GetBool(key, def) : def;
    }

    // 字符串数组参数（如 targets）。没有该键、或不是数组时返回空。
    // 单个元素不是字符串时跳过 —— 宁可少一个目标，也不要塞进去一个坏路径。
    std::vector<std::wstring> StrArray(const wchar_t* key) const {
        std::vector<std::wstring> out;
        const json::Value* a = v_.Find(key);
        if (a && a->IsArray())
            for (const auto& e : a->arr)
                if (e.IsString() && !e.str.empty()) out.push_back(e.str);
        return out;
    }

    // 原样取一个参数（不转型）。给"值类型由数据决定"的命令用 ——
    // 典型是 config.set：value 是布尔还是整数取决于 key，用 Bool/Int 取
    // 会把"给了字符串"静默变成 0/false。返回 nullptr = 没传这个键，
    // 与"传了空值"必须区分（前者该报参数缺失，后者该被注册表校验拒绝）。
    const json::Value* Any(const wchar_t* key) const { return v_.Find(key); }

private:
    json::Value v_;
};

// ---------------------------------------------------------------- 效果描述
//
// 从"单轴风险等级"改成"多个正交特性"（业界共识 P3）。
//
// 为什么单轴不够：`CmdPaste`（写、不可撤销）、`NavRefresh`（写、幂等）、
// `OpenTerminal`（写、作用于进程外）全归到一个 Risk::Write，但安全策略完全不同：
//   · 幂等的操作失败可以安全重试；
//   · 不可撤销的操作必须先确认；
//   · 作用于进程外的操作不能靠"读只读"简单放行。
// 一个枚举表达不了三件事，硬塞进去就只能是"都算危险"或"都算安全"。
//
// 这些是 **hint（声明）而非保证**。MCP 规范明确要求调用方把 annotations
// 视为不可信 —— 所以真正的权限判定还要叠加调用方身份、设置开关与审批，
// 不能只看这里。

// 影响范围：决定"需不需要点名目标"。
enum class Scope {
    CurrentTab,   // 作用于当前标签（导航、排序、视图）
    App,          // 作用于整个窗口（切标签、显隐栏、命令面板）
    System,       // 作用于进程外（启动终端、写注册表、弹系统对话框）
};

struct Effects {
    bool readOnly = false;     // 不改任何状态（不改磁盘、视图、选区、焦点、剪贴板）
    bool destructive = false;  // 可能不可逆：删除、覆盖、批量改名
    bool idempotent = false;   // 同参数重复调用等价（刷新、排序= 是；粘贴、新建= 否）
    bool openWorld = false;    // 作用于进程外：启动程序、访问网络、系统对话框
    bool undoable = false;     // 进撤销栈（我们能撤销的很少：书签、标签关闭）
    Scope scope = Scope::CurrentTab;

    // 由上面几项推导出来的"是否只是读"。不单独存，避免两处真相不一致。
    bool IsPureRead() const { return readOnly && !destructive && !openWorld; }
};
