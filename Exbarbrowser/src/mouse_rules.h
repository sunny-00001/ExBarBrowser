// 鼠标规则：目标区 × 按钮 × 触发方式 × 修饰键 -> 动作。
//
// 数据语义与 mod 的 `Bindings\mouse:*` 一致（target/button/trigger/mods/action/command），
// 但**不装低级鼠标钩子**：窗口是我们自己的，组件在命中测试后把 (目标区, 按钮, 触发)
// 交给 MouseRouter，由它查规则；没有规则才走组件的内置行为。
#pragma once
#include <string>
#include <vector>

#include "common.h"  // WPARAM / VK_XBUTTON1/2、GetKeyState 等 Win32 类型与 API

enum MouseTarget {
    MT_UNKNOWN = -1,     // 非可配置区域（预览窗格/滚动条/未知元素），不匹配规则
    MT_EMPTY = 0,        // 文件列表空白
    MT_FILE,             // 文件项
    MT_FOLDER,           // 文件夹项
    MT_TAB,              // 标签
    MT_TAB_CLOSE,        // 标签关闭按钮
    MT_NEW_TAB,          // "+"
    MT_BOOKMARK,         // 书签
    MT_BOOKMARK_FOLDER,  // 书签分类
    MT_BOOKMARK_EMPTY,   // 书签栏空白
    MT_NAV,              // 导航栏按钮
    MT_ADDRESS,          // 地址框
    MT_STATUS,           // 状态栏空白
    MT_STATUS_SEG,       // 状态栏段
    MT_VIEW,             // 视图区
    MT_TITLE,            // 标题栏
    MT_OVERFLOW,         // 溢出按钮
    // Shell 文件视图细分目标（与 Exbar.cpp 的 TARGET_GROUP/COLUMN/TREE 对齐）
    MT_GROUP,            // 分组头（UIGroupItem）
    MT_COLUMN,           // 列标题（HeaderItem）
    MT_TREE,             // 导航窗格树（SysTreeView32）
    MT_SEARCH,           // 搜索框
    MT_TOOLBAR,          // 命令栏/工具栏
    MT_PREVIEW,          // 预览窗格
    MT_COUNT
};

enum MouseButton { MBTN_LEFT = 0, MBTN_RIGHT, MBTN_MIDDLE, MBTN_X1, MBTN_X2, MBTN_COUNT };
enum MouseTrigger {
    TR_SINGLE = 0,
    TR_DOUBLE,
    TR_HOLD,
    TR_WHEEL_UP,
    TR_WHEEL_DOWN,
    TR_WHEEL_LEFT,
    TR_WHEEL_RIGHT,
    TR_COUNT
};

struct MouseRule {
    int target = MT_EMPTY;
    int button = MBTN_LEFT;
    int trigger = TR_SINGLE;
    int mods = 0;  // 1 Ctrl / 2 Shift / 4 Alt
    std::wstring action = L"none";  // command / none / runcmd / sendkeys
    std::wstring command;           // action=command 时的命令名
    std::wstring hotkey;            // sendkeys 用（不实现，保留字段）
    bool builtin = false;
};

const wchar_t* MouseTargetName(int target);
const wchar_t* MouseButtonName(int button);
const wchar_t* MouseTriggerName(int trigger);
int MouseTargetFromName(const std::wstring& name);
int MouseButtonFromName(const std::wstring& name);
int MouseTriggerFromName(const std::wstring& name);

// 当前修饰键位（1 Ctrl / 2 Shift / 4 Alt），与 MouseRule::mods 及 data/mouse.json 同编码。
//
// 【为什么在输入端而不是 UI 层】这个函数读 GetKeyState —— 那是**输入状态**，
// 不是绘制资源。以前它放在 ui_draw 的旧命名空间（eb，已废弃）里，于是每个栏在自己
// 的 WM_MOUSEMOVE 里调 UI 层的函数去问输入状态，分层的方向是反的
// （ARCHITECTURE-RULES.md R15：输入端只产命令，UI 只画）。
//
// 返回 0 有两个含义：没按修饰键，或按了 AltGr（右 Alt + 左 Ctrl —— 德语/波兰语等
// 布局下 AltGr+E 是字符「ę」，不能被当成 Ctrl+Alt+E 吃掉）。调用方按"无修饰"处理。
int CurrentMods();

// WM_XBUTTONDOWN/UP 的 wParam → MBTN_X1/MBTN_X2。wParam 的 HIWORD 携带侧键编号
// （XBUTTON1/XBUTTON2），但经 WM_PARENTNOTIFY 上抛的消息不带编号 —— 此时用
// VK_XBUTTON1/2 的物理按下状态兜底，口径与 shell_input 的 ButtonFrom 完全一致。
// 各自绘栏（标签/导航/书签/状态）处理 XBUTTON 消息时统一走这里，避免四份解析漂移。
// 无编号且物理状态也判不出时返回 -1（调用方放行消息）。
int XButtonFromWParam(WPARAM w);

class MouseRules {
public:
    static MouseRules& Get();

    void Load(const std::wstring& path);  // 失败 -> 内置默认
    void LoadDefaults();

    // mods 精确匹配；同一组合出现多条时，后加载的（用户配置）优先
    const MouseRule* Find(int target, int button, int trigger, int mods) const;
    const std::vector<MouseRule>& all() const { return rules_; }

    // 规则存在性早退：Shell 视图的 UIA ElementFromPoint 是跨进程 RPC，
    // 没有对应按键/滚轮规则时必须零开销放行（右键卡顿的关键）。
    bool HasPointerRules(int button) const;  // single / double / hold
    bool HasWheelRules() const;
    bool HasTriggerRules(int button, int trigger) const;

private:
    std::vector<MouseRule> rules_;
};
