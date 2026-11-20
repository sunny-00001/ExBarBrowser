// 鼠标规则：目标区 × 按钮 × 触发方式 × 修饰键 -> 动作。
//
// 数据语义与 mod 的 `Bindings\mouse:*` 一致（target/button/trigger/mods/action/command），
// 但**不装低级鼠标钩子**：窗口是我们自己的，组件在命中测试后把 (目标区, 按钮, 触发)
// 交给 MouseRouter，由它查规则；没有规则才走组件的内置行为。
#pragma once
#include <string>
#include <vector>

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
