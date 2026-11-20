// 配置与状态：路径解析 + 读写 + 最近访问 + 会话 + 窗口几何 + 输入覆盖。
//
// 【存储位置：便携模式】所有用户数据写在 exe 同目录的 data\ 下：
//   data\settings.json          本工程设置（栏显隐/视图/窗格/几何/会话/最近访问/键位覆盖）
//   data\bookmarks.html         书签（Netscape + EXBAR_*）—— 用户数据
//   data\keymap.json            键位（直接编辑；缺失时回落到编译期内置默认）
//   data\mouse.json             鼠标规则（同上）
//   data\audit.log              命令审计
//
// 取舍说明：
//   · 整个程序目录可整体拷走，不写注册表、不写 %APPDATA%，便于随身/绿色部署；
//   · 书签不与 ExBar mod 共用 %APPDATA%\ExBar\bookmarks.html —— 两边各写各的
//     才不会互相覆盖；需要共用时请手工拷贝；
//   · 若 exe 放在 Program Files 这类无写权限的位置，配置写不进去（界面照常可用，
//     但改动不会落盘）；此时请把整个目录放到普通用户可写的位置。
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "json.h"

// 键位覆盖里表示"显式解绑"的哨兵值。空字符串做不到这件事：
// BuildBindings 见到空键会退到系统默认键，用户点了"清除"却发现键又回来了。
constexpr wchar_t kHotkeyDisabled[] = L"-";

struct Settings {
    // 栏显隐（对应 mod 的 tab / nav / bookmark / statusbar）
    bool showTab = true;
    bool showNav = true;
    bool showBookmark = true;
    bool showStatus = true;

    // 序列键
    bool seqEnabled = true;
    int seqTimeoutMs = 3000;
    bool seqShowHints = true;
    bool seqSingleInstant = true;
    bool seqUniqueAuto = true;

    // 鼠标长按阈值（mod 默认 800，且必须大于双击时间）
    int holdDelayMs = 800;

    std::wstring tabNewPos = L"right";

    // 视图：8 档模式下标（0..7，见 app_commands.cpp 的 kViewModes）、排序与分组
    int viewMode = 3;
    int sortMode = 0;      // 0名称 1日期 2类型 3大小
    bool sortAscending = true;
    int groupBy = 0;       // 0无 1类型 2日期 3大小

    // 窗格显隐（每个标签启动时套用）
    bool showNavPane = true;
    bool showPreviewPane = true;

    // 窗口几何
    bool hasGeometry = false;
    int winX = 0, winY = 0, winW = 1100, winH = 760;
    bool maximized = false;

    // 会话 / 历史
    std::vector<std::wstring> recentPaths;   // 最近访问，front = 最新
    std::vector<std::wstring> sessionPaths;  // 上次关闭时的标签
    int sessionActive = 0;
    std::vector<std::wstring> lockedPaths;   // 锁定标签（按路径持久化）
    std::vector<std::wstring> closedTabs;    // 关闭历史（可重开）

    // 状态栏段显隐（段号见 status_bar.h 的 SegId）
    std::vector<int> statusSegs{0, 1, 2, 3, 4, 5, 6};

    // 快捷键覆盖：setting -> "Ctrl+Shift+T"（空串 = 显式禁用）
    std::vector<std::pair<std::wstring, std::wstring>> hotkeyOverrides;

    // ---- AI 操作通道 ----
    // 默认关闭：这是"让外部程序驱动本窗口"的能力，不该默认对外开放。
    // 开启后监听一个本地命名管道（名与令牌见 data\ai-pipe.txt / ai-token.txt，
    // 停服即删）。人机等价：管道能做的每件事人也能做，命令表是唯一真源。
    bool aiEnabled = false;
    // 是否允许 AI 执行 Risk::Danger 命令。关闭时这类命令一律拒绝（即使调用方
    // 带 approve=true）—— 相当于"人类保留最终授权"的硬开关。
    bool aiAllowDanger = false;
};

class Config {
public:
    static Config& Get();

    void Load();
    void Save();          // 全量写盘（调用点很少：改配置、关窗口）

    const Settings& s() const { return s_; }
    Settings& mutableSettings() { return s_; }

    // 路径
    std::wstring Dir() const;          // exe\data（便携模式，见文件头说明）
    std::wstring SettingsPath() const;
    std::wstring BookmarksPath() const;
    std::wstring KeymapPath() const;
    std::wstring MousePath() const;
    std::wstring DataDir() const;      // 与 Dir() 相同：数据就放在 data\ 里

    void AddRecent(const std::wstring& path);
    void RememberSession(const std::vector<std::wstring>& paths, int active);
    void SetHotkeyOverride(const std::wstring& setting, const std::wstring& key);
    void ClearHotkeyOverride(const std::wstring& setting);
    std::wstring HotkeyOverride(const std::wstring& setting) const;
    bool HasHotkeyOverride(const std::wstring& setting) const;

private:
    Config() = default;
    mutable std::wstring dir_;
    Settings s_;
    bool loaded_ = false;
};
