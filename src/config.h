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
#include "tab_set.h"   // core::kTabColorCount：TabMeta::color 的取值上界

// 键位覆盖里表示"显式解绑"的哨兵值。空字符串做不到这件事：
// BuildBindings 见到空键会退到系统默认键，用户点了"清除"却发现键又回来了。
constexpr wchar_t kHotkeyDisabled[] = L"-";

// 标签个性化（改名 / 颜色），**按路径**持久化。
//
// 为什么不按标签序号：序号每次启动都会重排（会话恢复顺序、关掉再开都会变），
// 按序号存等于每次重启都错位到别的标签上。
// 不按标签 id：id 是进程内自增，跨会话没有意义。
// 路径是唯一稳定的键 —— 与 lockedPaths 同一取舍（锁定也是按路径持久化的）。
struct TabMeta {
    std::wstring path;
    std::wstring alias;   // 空 = 未改名
    int color = 0;        // 0 = 无颜色
};

// 单标签的导航历史（会话持久化用）。paths 是完整栈，idx 是当前游标。
// 只存路径字符串：重启后由 History::Restore 装填，不重放 Shell 导航。
struct TabHistory {
    std::vector<std::wstring> paths;
    int idx = 0;
};

// 命名会话（P2：多条标签组的保存/列出/恢复/删除）。
// 不含活动下标与选中项：恢复 = 整组重开，恢复后的活动标签由恢复时序决定。
struct NamedSession {
    std::wstring name;
    std::vector<std::wstring> paths;
};

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
    // 关闭窗口且标签数 > 1 时先确认。默认开：多标签是一次上下文，
    // 误按 Alt+F4 的代价比多一次点击高。最后一个标签关闭（= 退出）不弹。
    bool confirmCloseTabs = true;
    // 标签宽度（逻辑像素）。默认 112–230 与旧行为一致；紧凑=80/160，
    // 宽松=140/320。固定标签不吃这两个值（恒为图标宽）。
    int tabMinWidth = 112;
    int tabMaxWidth = 230;

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
    std::vector<std::wstring> sessionSelection;  // 关闭时活动标签的选中项（重启恢复用）
    // 与 sessionPaths 一一对应的每标签历史（可缺省：旧配置没有这一段）。
    std::vector<TabHistory> sessionHistory;
    // 命名会话：多条标签组（session.save/list/restore/delete 操作的是"当前那条"，
    // named.* 命令按名字操作这里的条目）。按名字覆盖保存。
    std::vector<NamedSession> namedSessions;
    std::vector<std::wstring> lockedPaths;   // 锁定标签（按路径持久化）
    std::vector<std::wstring> pinnedPaths;   // 固定标签（按路径持久化）
    std::vector<TabMeta> tabMeta;            // 标签改名/颜色（按路径持久化）
    std::vector<std::wstring> closedTabs;    // 关闭历史（可重开）

    // 状态栏段显隐（段号见 status_bar.h 的 SegId）
    std::vector<int> statusSegs{0, 1, 2, 3, 4, 5, 6};

    // 快捷键覆盖：setting -> "Ctrl+Shift+T"（空串 = 显式禁用）
    std::vector<std::pair<std::wstring, std::wstring>> hotkeyOverrides;

    // ---- AI 操作通道 ----
    // 默认开启：这是"让外部程序驱动本窗口"的能力，人机等价（命令表是唯一真源）。
    // 开启后监听一个本地命名管道（名与令牌见 data\ai-pipe.txt / ai-token.txt，
    // 停服即删）。**支持热插拔**：config.set aiEnabled 会立即启停服务，无需重启
    // （见 App::SyncAiChannel）；关闭后不再监听任何连接。
    bool aiEnabled = true;
    // 是否允许 AI 执行 Risk::Danger 命令。关闭时这类命令一律拒绝（即使调用方
    // 带 approve=true）—— 相当于"人类保留最终授权"的硬开关。
    bool aiAllowDanger = false;
};

class Config {
public:
    static Config& Get();

    // 指定用户数据目录。必须在第一次 Load/Save 之前调用（main 启动时调用）。
    // 改目录会重置内存配置，保证不会把 A 目录的设置写进 B 目录。
    void SetProfileDir(const std::wstring& dir);
    void Load();
    void Save();          // 全量写盘（调用点很少：改配置、关窗口）

    const Settings& s() const { return s_; }
    Settings& mutableSettings() { return s_; }

    // 启动时强制开启 AI 通道（来自 --ai / EXBARBROWSER_AI）。
    // 【为什么不塞进 Settings】那是会被 Save() 全量写回 settings.json 的；
    // 一旦写回，测试实例关闭就把用户的 aiEnabled 永久翻成 true —— 正是要消灭的污染。
    // 这个标志纯进程内：只参与"要不要 StartServer"的判断，永不落盘。
    // 热插拔时若用户显式设置了 aiEnabled，会先清掉它（人的动作优先于测试开关）。
    void SetAiForceEnabled(bool v) { aiForceEnabled_ = v; }
    bool AiChannelEnabled() const { return aiForceEnabled_ || s_.aiEnabled; }

    // 路径
    std::wstring Dir() const;          // exe\data（便携模式，见文件头说明）
    std::wstring SettingsPath() const;
    std::wstring BookmarksPath() const;
    std::wstring KeymapPath() const;
    std::wstring MousePath() const;
    std::wstring DataDir() const;      // 与 Dir() 相同：数据就放在 data\ 里

    void AddRecent(const std::wstring& path);
    void RememberSession(const std::vector<std::wstring>& paths, int active,
                         const std::vector<std::wstring>& selection = {},
                         const std::vector<TabHistory>& history = {});
    // 标签个性化（改名/颜色）的写入口。path 是定位键，alias 空 + color 0 = 删除该项。
    // 只改内存；落盘时机与其它会话状态一致（App 退出流程统一 Save）。
    void SetTabMeta(const std::wstring& path, const std::wstring& alias, int color);
    // 读：找不到返回默认（空 alias、color 0）。
    void GetTabMeta(const std::wstring& path, std::wstring* alias, int* color) const;
    void SetHotkeyOverride(const std::wstring& setting, const std::wstring& key);
    void ClearHotkeyOverride(const std::wstring& setting);
    std::wstring HotkeyOverride(const std::wstring& setting) const;
    bool HasHotkeyOverride(const std::wstring& setting) const;

private:
    Config() = default;
    mutable std::wstring dir_;
    Settings s_;
    bool loaded_ = false;
    // 进程内启动开关，不随 SetProfileDir 重置（它在 main 每次启动只设一次），也不落盘。
    bool aiForceEnabled_ = false;
    // settings.json 存在但解析失败 → 禁止 Save 覆盖（否则用户手改打错一个逗号，
    // 下次任何一次 Save 都会把 recentPaths / sessionPaths / lockedPaths / closedTabs /
    // hotkeyOverrides 全部抹掉）。见 config.cpp Load() 的注释。
    bool parseFailed_ = false;
};
