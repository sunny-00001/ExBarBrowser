// 「有效键位」的唯一推导实现：把键位表 + 用户覆盖推导成**按下真能触发**的一组组合键。
//
// 【为什么必须抽出来】这件事以前只写在 InputRouter::BuildBindings() 里，于是
// "谁按得出来"只有输入端知道。菜单要在行右端显示快捷键（Win32 / Explorer /
// Win11 Fluent / VS Code 的标准做法）时，只有两条路：
//   · 再写一套"从 keymap 推键位"的逻辑 → 第二套真相。用户改了键、或某条命令
//     因保留键被剔除，菜单里就会显示一个**按了没反应**的键。这种谎最难发现：
//     它永远"看起来对"。
//   · 把推导抽成纯函数，输入端与菜单都调它 → 显示的键与生效的键同源。
//
// 【为什么是纯函数】推导只依赖 (Keymap, 覆盖表) 两个入参，不碰窗口/不读注册表，
// 于是可以单测：喂一份键位表，断言"保留键被剔除""先到先得""未实现命令不入表"。
// 这三条判据正是"显示与生效不一致"的全部成因。
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "chord.h"
#include "commands.h"   // CommandDef / HasRequiredArgs（判定"空参做不出这一行"）
#include "keymap.h"

// 用户覆盖表里表示"显式解绑"的哨兵值。空字符串做不到这件事：
// BuildBindings 见到空键会退到系统默认键，用户点了"清除"却发现键又回来了。
// 【归属】这是键位推导（本模块）的契约，不是配置存储的 —— input 层要用它，
// 又不能因此 include config.h（R15），故常量定在这里，config.h 反向转发。
constexpr wchar_t kHotkeyDisabled[] = L"-";

namespace keybinding {

// 一条生效的绑定。
struct Binding {
    KeyChord chord;       // 解析后的组合键
    std::wstring spec;    // 命令规格，可能带内联参数（"ViewMode:3"）
    std::wstring setting; // 配置键（hotkeyXxx），"清除"覆盖以它为键
    std::wstring scope;   // UI 作用域（CommandDef::scopeUi），"标签栏裸键"判定要用
};

// 用户覆盖表：setting -> key。哨兵 kHotkeyDisabled 表示**显式解绑**
// （不能回落到系统默认键，否则用户点了"清除"却发现键又回来了）。
using Overrides = std::vector<std::pair<std::wstring, std::wstring>>;

// 单条键位的**当前生效文本**（"Ctrl+Shift+T"），空串 = 这条命令现在按不出来。
//
// 【什么时候需要它】Effective 给的是"一组组合键"，用于判定谁能触发；
// 而快捷键管理器要逐行显示**配置里那一条**写成什么（包括显式解绑的"-"）。
// 两者用的是同一条解析规则（覆盖优先、空则回落系统原生键、哨兵=解绑），
// 所以规则本身只能有一份 —— 这里就是它的单条形态，Effective 内部也调它。
//
// 返回值约定（调用方据此决定显示"（未绑定）"还是键名）：
//   · 用户覆盖为哨兵 kHotkeyDisabled → 返回 kHotkeyDisabled（"-"），表示显式解绑；
//   · 否则返回覆盖值 / 表内值 / 系统原生键中第一个非空者；都没有则空串。
std::wstring ResolveKey(const std::wstring& setting, const std::wstring& keyInTable,
                        const Overrides& overrides);

// 推导生效组合键集合。规则（顺序即优先级）：
//
//   1. 覆盖表优先于键位表默认值；哨兵值 = 解绑，跳过；
//   2. 空键回落到系统原生键（SystemKeyFor）；
//   3. 系统保留键（chord::IsReserved）剔除 —— 它们永远轮不到本程序；
//   4. 命令未实现 / 名字查不到 → 跳过（显示了也执行不了）；
//   5. **先到先得**：同一组合键第二次出现即丢弃（与 mod 的 orderDispatch 同口径）。
std::vector<Binding> Effective(const Keymap& km, const Overrides& overrides);

// 按**命令身份**取显示文案 —— 菜单唯一的取数入口。
//
// 【为什么菜单侧只认 CmdId，不给它一个字符串接口】字符串接口有两类固定的错，
// 而且都不会在编译期暴露：
//   1. 拼错命令名（写 `tab.colse`）—— 解析时找不到，菜单右端静默留空，
//      看起来"这条命令没绑键"，实际是写错了；
//   2. 指的是**另一条命令**（写 `path.copy.dir` 而这一行复制的是某个标签的路径）
//      —— 显示出来的键按下去作用在别处，属于本项目最不能容忍的谎。
// 名字由 FindCommand(id) 反查，调用点只写枚举：拼错不行、指错也会在代码评审里
// 一眼看出"这一行是 TabClone 还是 PathCopy"。命令改名/删除时编译器直接报。
//
// arg 是**内联参数**（"3" = 第 4 档）。空 = 该行不带参数。
// 参数必须逐字相符才认（见 TextFor 的说明）：宁可留空，也不显示一个作用于别档的键。
std::wstring TextForCmd(CmdId id, const std::wstring& arg, const Keymap& km,
                        const std::vector<Binding>& chords, bool allowSequences);
std::wstring TextForCmd(CmdId id, const std::wstring& arg, const Keymap& km,
                        const Overrides& overrides, bool allowSequences);

// 命令规格 → 菜单右端要显示的键位文案。空串 = 这条命令当前没有可用快捷键。
//
// 匹配口径：spec 拆成 (名字, 内联参数) 后经 cmdnaming::CanonicalName 归一，
// 于是 `view.mode` + 参数 "3" 与键位表里的 `ViewMode:3` 是同一条 —— 菜单行
// 只写规范名，键位表里留的是 legacy 名，两边都不需要互相认识。
//
// 顺序：组合键优先（一次按完，比两段序列更值得提示）；没有组合键时退回
// **序列键**（"t b"）。序列键也是"真能触发"，显示它同样是真话 —— 但序列受
// seqEnabled 开关控制，关掉时不能显示（否则就是"按了没反应"的谎），
// 所以 allowSequences 由调用方按当前配置给。两样都没有才返回空串。
//
// 为什么带参命令要**参数也对上**：键位表里 view.mode 有 8 档（:0..:7）。
// 只比名字会让 8 行菜单全显示同一个键（"Ctrl+Shift+1"），那是纯粹的错。
std::wstring TextFor(const std::wstring& spec, const Keymap& km,
                     const std::vector<Binding>& chords, bool allowSequences);

// 便利版：自己推一遍 Effective（菜单只显示一次、不需要复用集合时用它）。
std::wstring TextFor(const std::wstring& spec, const Keymap& km, const Overrides& overrides,
                     bool allowSequences);

// 「这个键按下后做不出这一行的效果」判定（Effective / TextFor 内部用它挡谎）。
//
// 【为什么需要单独一条】键位表里的键按下后走**空参**路径
// （InputRouter → InvokeByNameLegacy(cmd, L"")）。对带必填参数的命令来说，
// 那个键做的不是这一行的事：
//   · clipboard.copy 空参 = 把**空串**写进剪贴板（毁掉用户上一条复制内容）；
//   · fs.open / fs.properties / ext.terminal.at 空参 = 直接失败；
//   · bmk.export / bmk.import 空参 = 缺 path/mode，失败。
// 这类键既不能进"生效表"，也不能显示在菜单右端。
//
// inlineArg 非空（"ViewMode:3"）表示键自带参数 —— 它确实做这一行的事，不放挡。
//
// 【为什么是公开的】它同时被 key_binding 内部（Effective / TextFor 挡谎）与单测
// 使用。判据必须能在不建窗口的前提下被断言 —— 否则"这条键会不会做错事"就只能
// 靠手点菜单发现，而这类谎恰恰最难在界面上看出来。
bool ActingWithoutArgs(const CommandDef& def, const std::wstring& inlineArg);

}  // namespace keybinding