// 设置项 -> "改完必须做什么"的判据表（纯函数，可单测）。
//
// 【为什么必须抽出来单测】这套判据以前散在 CommandSet 里的一串 if/else if，
// 漏一个键的后果是**命令返回 ok=true、界面一点没变** —— 2026-11-23 实测：
// `config.set bookmark=false` 写盘成功、`config.get` 读回 false，而书签栏
// 仍然 visible=True、高 26px。这类"假成功"没有任何检查会报，只能靠一张
// 显式的表 + 用例钉住"每个可写键都必须被某个落地类别覆盖"。
//
// 【判据为什么按"影响面"分类，而不是按命令名列举】
// 命令名是会增长的（这轮就新增了 5 个），而影响面只有四类：
//   布局（客户区怎么分） / 视图（套到当前 IShellView） / 主题（重建字体画刷）
//   / 历史（裁到新上限）。新键归类时只需要回答"它动的是哪一块"。
#pragma once

#include <string>

namespace cfgapply {

// 会影响**客户区空间分配**：哪些栏在、各占多高、导航窗格占多宽、双栏怎么切。
// 这一类改完必须 Layout()，否则栏不会真的显隐/改尺寸。
bool AffectsLayout(const std::wstring& key);

// 会影响**当前视图的外观状态**：视图档位、排序、分组。
// 这一类改完必须套到活动 ShellView（ApplyPersistedViewState）。
bool AffectsActiveView(const std::wstring& key);

// 会影响**状态栏的段集**。段集变了要重新喂给状态栏（它自己会重排内层几何）。
bool AffectsStatusBar(const std::wstring& key);

// 需要重套主题（字体/画刷/四条栏配色全部重建）。
bool AffectsTheme(const std::wstring& key);

// 需要把历史上限推到"真正持有历史的那两个模型"上（并顺手裁剪已有条目）。
bool AffectsHistoryLimits(const std::wstring& key);

// 会影响**快捷键管理器面板的几何**（行高 / 工具栏高 / 左右栏宽）。
// 这一类改完必须让它重排（ReloadTheme → Layout），否则"config.set 返回 ok、
// 界面还是原样"。窗口宽高不在其中：那只影响下次打开的初始尺寸，
// 去改一个已经打开的窗口等于覆盖用户自己调的大小。
bool AffectsHotkeyPanel(const std::wstring& key);

// 这一项在改完之后是否需要**任何**落地动作（布局/视图/主题/历史/状态栏）。
// 表驱动用例用它做"全覆盖"判定：可写项若有 false 且不是纯数据项，就是漏了。
bool NeedsAnyApply(const std::wstring& key);

// 纯数据项：只影响"下次启动/下次读取"的行为，改完当场没有可见效果，
// 因此不需要落地动作。这一类必须**逐个列名**，不允许"没有分支就算纯数据"——
// 后者会让"新加了一个键但忘了给它写落地"静默通过。
bool IsPureData(const std::wstring& key);

}  // namespace cfgapply