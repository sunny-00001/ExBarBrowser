// 搜索范围（导航栏搜索框左侧菜单）的唯一真源。
//
// 【为什么单独成模块】范围要同时落到三处：菜单里的勾选、搜索后端的作用域、
// 命令参数（search.scope.set 的 scope）。三处各写一份必然漂移 ——
// 表现是"菜单写着『当前目录』，实际搜的是整台电脑"。
//
// 【范围 → 作用域】搜索后端是**原生搜索文件夹**（src/search_folder.h）：
// 它的作用域由两件事决定 —— 限定哪个目录、要不要含子目录。
// 以前这里拼的是 search-ms: URL 的 crumb 片段，但 URL 形式在本宿主根本
// 导航不了（ShellView::DoNavigate 拒绝 search-ms: 前缀，见 EXPLORER-GAP-AUDIT
// F1），crumb 就没人用了 —— 留着两套范围语义必然漂移。现在只落一次：
//   · kCurrent  仅当前目录本层（父目录恰好等于当前目录的项）
//   · kSubtree  当前目录及其子目录（默认）
//   · kThisPC   整台电脑 —— **本宿主做不到**（见下），Resolve 返回 valid=false
//   · kCustom   自定义路径及其子目录
//
// 【kThisPC 为什么做不到】原生搜索文件夹的作用域只能是一个普通文件系统
// 目录（约束与实测见 src/search_folder.h）：不给作用域、给命名空间项、给多个
// 目录的数组，shell 都绑不上（BrowseToObject 返回 S_OK，视图留在原地）。
// 真正的"整机搜索"是 Explorer 进程内的 search-ms 全局查询，本宿主没有那个宿主。
// 所以这一档是"明确报错"，不是"悄悄搜成别的范围"。
//
// 【不写兑现不了的选项】每一档都必须真的能搜出不同的范围：
// 拼不出来（当前目录未知 / 自定义路径没设）时 valid=false，调用方据此报错，
// 而不是悄悄退回默认档 —— 那正是"菜单显示 A、实际搜 B"的来源。
#pragma once
#include <string>

namespace searchscope {

// 范围档位。数值会写进 state\tabs.json（searchRange），所以**只增不改**：
// 顺序即持久化格式，插在中间会让旧配置读到别的档位。
enum Range {
    kCurrent = 0,  // 仅当前目录本层
    kSubtree = 1,  // 当前目录及其子目录（默认）
    kThisPC = 2,   // 此电脑（整台机器）
    kCustom = 3,   // 自定义路径
};

constexpr int kRangeCount = 4;
// 默认档 = 含子目录：与本工程搜索框改造前的行为一致（crumb=location:<当前目录>，
// 递归是默认）。改默认值会让老用户第一次搜索的范围悄悄变小。
constexpr int kDefaultRange = kSubtree;

// 稳定 id（命令参数与持久化都用它，不用数字）：
// current / subtree / thispc / custom
const wchar_t* RangeId(int range);
// id → 档位。认不出返回 -1（调用方据此报错，不静默退回默认）。
int RangeFromId(const std::wstring& id);
// 菜单文案（勾选时展示的那一串）。
const wchar_t* RangeLabel(int range);
// 这一档在本宿主能不能真的搜出来。
// kThisPC = false（整机搜索要 Explorer 进程内的 search-ms 宿主，见头文件说明）。
// 界面据此把该行摆成灰字，命令据此直接拒绝 —— 不给"点了就失败"的入口。
bool RangeSupported(int range);
// 档位的短标签（标题/说明里用得上的那一小截）：
// 仅本层 / 含子目录 / 此电脑 / 自定义。
const wchar_t* RangeShortLabel(int range);

// 一档范围落到搜索后端的样子。
struct Target {
    std::wstring scopeDir;       // 限定目录（空 = 整台电脑的索引）
    bool onlyThisLayer = false;  // 是否只搜 scopeDir 本层
    bool valid = false;          // 这一档现在能不能用（自定义路径没设 = 不能用）
};

// 范围档位 → 搜索作用域。
// currentDir = 当前目录路径；customPath = 自定义范围路径（仅 kCustom 用）。
Target Resolve(int range, const std::wstring& currentDir, const std::wstring& customPath);

}  // namespace searchscope
