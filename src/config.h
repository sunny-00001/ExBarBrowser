// 配置与状态：路径解析 + 读写 + 最近访问 + 会话 + 窗口几何 + 输入覆盖。
//
// 【存储位置：便携模式】所有用户数据写在程序根目录下，按"变更多快 / 谁能改"分层：
//   data\settings.json     宿主偏好与策略（栏/视图/窗格/主题/AI 开关/超时…）
//   data\aichat.json       AI 服务行为配置（端口/温度/白名单…）—— Node 服务独占
//   data\models.json       模型目录 + apiKey —— Node 服务独占
//   data\mcp.json          外部 MCP server 清单（骨架，暂不连接）
//   data\bookmarks.html    书签（Netscape + EXBAR_*）
//   data\keymap.json       键位（缺失时回落到编译期内置默认）
//   data\mouse.json        鼠标规则（同上）
//   state\session.json     标签/活动项/选中/双栏/每标签历史栈/命名会话
//   state\history.json     最近访问 / 搜索历史 / 关闭标签 / 搜索域
//   state\tabs.json        锁定 / 固定 / 标签别名颜色 / 状态栏段 / 搜索域 / 热键覆盖
//   state\window.json      窗口几何（崩溃前最多丢 sessionSaveDelayMs）
//   logs\audit.jsonl       命令审计（规范格式）+ audit.log（legacy 镜像）
//   logs\events.jsonl      事件流（与审计同一 trace）
//   logs\sessions\         AI 对话历史
//   cache\webview2\        WebView2 用户数据（可整体删，重建无损失）
//   runtime\ai-*.txt       AI 通道发现文件（进程退出即删）
//
// 【为什么分开】settings.json 是低频改、人工可读的偏好；state\* 是高频写、
// 程序接管的状态。混在一个文件里时，改一个栏显隐会把整份会话/历史重写一遍，
// 一处解析失败就要禁止整份写盘（连带历史全丢）。分层后两者互不拖累。
//
// 【根与 profile】RootDir() 默认是 exe 目录；EXBARBROWSER_PROFILE 显式指定时
// 是用户给的 profile 根。所有子目录都在根下复刻，多实例全套隔离。
// data/ 保持扁平（不放子目录），其余四类与 data/ 平级。
//
// 取舍说明：
//   · 整个程序目录可整体拷走，不写注册表、不写 %APPDATA%，便于随身/绿色部署；
//   · 书签不与 ExBar mod 共用 %APPDATA%\ExBar\bookmarks.html —— 两边各写各的
//     才不会互相覆盖；需要共用时请手工拷贝；
//   · 若 exe 放在 Program Files 这类无写权限的位置，配置写不进去（界面照常可用，
//     但改动不会落盘）；此时请把整个目录放到普通用户可写的位置。
#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "key_binding.h"   // kHotkeyDisabled（哨兵唯一来源；定义在键位推导层，这里转发）
#include "layout_limits.h"   // 栏宽上下限与默认值（与 UI 拖动范围同一份）
#include "tab_set.h"   // core::kTabColorCount：TabMeta::color 的取值上界

// 状态栏段集合的**规范形**：升序 + 去重。
//
// 【为什么必须只有一份】这个数组有两个来源（config.set / 手改 settings.json），
// 而"规范形"以前只写在读盘那一侧：写入端原样存、读盘端排序去重，于是
// `config.set statusSegs [6,0,6]` 会被判成功（回读确实等于请求），
// 下次启动变成 [0,6]。同时写在两处又必然漂 —— 所以只有这一个实现，
// 两边都调它（写入端存规范形、声明同一份 canon；读盘端修手改文件）。
//
// 去重/升序不只是美观：状态栏右对齐的排版按数组顺序走，重复段会被画两遍，
// 而"这条状态怎么出现了两次"完全无法反推到配置里的数组。
inline void NormalizeStatusSegs(std::vector<int>& segs) {
    std::sort(segs.begin(), segs.end());
    segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
}

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

    // 四条栏的高度（96 DPI 逻辑像素，Theme::S 负责缩放）。此前它们是
    // Theme 里的四个 static constexpr，用户想让标签栏紧凑一点只能改代码重编译。
    // 现在每项独立可调，并且都进 AffectsLayout：改完立即重排整窗。
    int tabBarHeight = 26;
    int navBarHeight = 30;
    int bookmarkBarHeight = 26;
    int statusBarHeight = 24;

    // 各栏自己的手感项（都放在 bars / view 分组，避免"只有显隐算设置"）。
    int navBarSearchWidth = 200;       // 导航栏搜索框总宽
    int navBarAddressMinWidth = 160;   // 地址框低于此宽就收起搜索框
    int bookmarkBarTooltipWidth = 600; // 书签提示窗最大宽
    // 书签栏滚轮一格走多远（逻辑像素）。以前写死 48（≈三个字宽）：
    // 书签条目宽窄差异大，"一格跳过一条"与"一格挪一点点"都有人要。
    int bookmarkWheelStepDp = 48;      // 书签栏滚轮步进（16..200）

    // 书签栏点击打开语义（Chrome/Edge/Firefox 书签栏的通用标准，见
    // docs/BOOKMARK-BAR-AUDIT-2026-11-26.md §五）：
    //   中键 = 新标签打开、Ctrl+左键 = 新标签、Shift+左键 = 新窗口 —— 三家一致，
    //   属于肌肉记忆，本项目此前全部缺失（点哪都只进当前标签）。
    // openInNewTab 是 Edge 110+ 的「固定打开新标签页」：习惯固定不改浏览器的人
    // 不该被迫每次按 Ctrl。修饰键永远反向覆盖它（开了开关 Ctrl 反而回当前标签），
    // 否则开关一开，想偶尔原地打开的人就没有退路了。
    bool bookmarkOpenInNewTab = false; // 书签固定在新标签打开（false = 进当前标签）
    // 拖拽到栏边缘自动滚动：书签栏与标签栏此前共用 tabDragScrollMs 一个周期
    // 旋钮，想"标签滚快点、书签滚慢点"做不到；且没有总开关（有人只想要滚轮）。
    // 开关是书签栏独立的 —— 标签栏从来没有开关需求，不陪绑。
    bool bookmarkDragAutoscroll = true; // 书签栏拖拽边缘自动滚动总开关
    int bookmarkDragScrollMs = 30;      // 书签栏拖拽自动滚动周期 ms（10..200）

    int statusBarTooltipWidth = 520;   // 状态栏提示窗最大宽
    int navPaneItemHeight = 30;        // 左侧导航树行高
    int navPaneIndent = 30;            // 左侧导航树图标缩进

    // 设置窗口自身三栏的尺寸。用户抱怨"输入框大小不统一、每栏没有自己的设置"
    // 时，这三项让左分类栏 / 右说明栏 / 值控件宽度都能单独调整；值控件统一用
    // settingsControlWidth，所以调宽后所有数字框/下拉/字符串框仍然一样宽。
    int settingsGroupWidth = 200;
    int settingsDetailWidth = 204;
    int settingsControlWidth = 192;

    // 快捷键管理器面板的尺寸。这几项以前是 hotkey_manager_layout.cpp 里的
    // constexpr —— 用户能看到的界面却调不动，是"设置项该有却没有"的典型。
    // 逻辑像素（96 DPI），实际绘制走 Theme::S() 换算。
    //
    // 【为什么行高只有一项】中栏行、左栏筛选项、表头、工具栏、状态栏**五处**
    // 共用一个值。以前是 30/32/26 三个数各写一遍，同一屏里三种行距 —— 眼睛扫
    // 下来觉得列表歪了，而代码里毫无异常。要"行距不齐"这类缺陷从根上不可能
    // 发生，就得让它只有一个数可改。
    //
    // 工具栏曾经有自己的高度（hkToolbarHeight，默认 40），结果是顶栏比列表每
    // 一行都高，两套节奏叠在同一屏里 —— 看着就是"工具栏胖了"。现在它也吃这一
    // 个数：想让整体更疏就调 hkRowHeight，五处一起变。
    int hkWindowWidth = 1120;
    int hkWindowHeight = 720;
    int hkRowHeight = 28;
    int hkRailWidth = 190;
    int hkDetailWidth = 270;

    // 深浅色：system = 跟随系统（默认）、dark / light = 强制。
    //
    // 【为什么需要强制档】系统级深浅色是**全局**设置：晚上想用深色，得把整个
    // Windows 一起改。文件管理器有自己的使用场景（长时间扫目录、对着图片
    // 做浅色比对），强制某一档是这类工具的标配。取值用稳定标识而不是数字：
    // 落盘值会被人手改，中文或下标都撑不住一次枚举增删。
    std::wstring themeMode = L"system";   // system / dark / light

    // 序列键
    bool seqEnabled = true;
    int seqTimeoutMs = 3000;
    bool seqShowHints = true;
    bool seqSingleInstant = true;
    bool seqUniqueAuto = true;

    // 序列浮层的滚动手感。以前是 sequence_overlay_input.cpp 里的裸 3 / 8：
    // 滚轮每格滚几行、PgUp/PgDn 一次跳几行，都是纯偏好（没有技术上的最优值），
    // 写死就只能改代码重编译。下次打开浮层生效。
    int seqWheelRows = 3;   // 滚轮每格行数（1..10）
    int seqPageRows = 8;    // 翻页行数（2..30）

    // 通用弹层（菜单 / 面包屑下拉）的交互手感。
    // typeAheadResetMs：连续键入超过这个间隔就重开一个前缀，否则累积 —— 慢打字者
    //   需要比 900ms 更长的窗口；popupMaxRows：面包屑文件夹树弹层一屏最多几行，
    //   深目录会被截在默认值上（其它菜单的行数由各自调用点决定，不受此项影响）。
    int popupTypeAheadMs = 900;  // 弹层键入定位间隔（200..3000）
    int popupMaxRows = 20;       // 面包屑树弹层可见行上限（6..40）
    // 弹层里"按住拖动排序"的启动阈值（逻辑像素）。以前写死 6px：
    // 太小会把"点一下选行"误判成拖动，太大则拖不动 —— 这纯是手感，不是最优值。
    int popupDragThreshold = 6;  // 弹层拖动阈值（2..20）

    // 鼠标长按阈值（mod 默认 800，且必须大于双击时间）
    int holdDelayMs = 800;

    // ---- 历史上限 ----
    // 这三个上限以前是代码里的裸数字（config.cpp 的 30 / 20、tab_set.cpp 的 20），
    // 而它们直接决定"地址栏下拉里能看到多少条历史"和"Ctrl+Shift+T 能连撤几步"。
    // 上限不是性能问题（几十条字符串），是**用户习惯**问题：有人一天跑几十个
    // 目录、有人几乎只用收藏路径。写成常量就必须改代码重新编译，写进设置才叫可调。
    int recentLimit = 30;      // 最近访问目录上限（1..200）
    int searchLimit = 20;      // 搜索历史上限（1..100）
    int closedTabLimit = 20;   // 关闭标签历史上限（1..100，= Ctrl+Shift+T 能回退几步）
    // 每窗格导航历史栈深度（后退/前进最多几步）。此前写死在 nav_history.h 的
    // kDefaultMaxEntries=512，与 closedTabLimit（可重开几步）是两回事。栈会整份写进
    // state\session.json 的 sessionHistory，深度也决定配置体积。
    int historyDepth = 512;    // 每窗格后退栈深度（50..2000）
    // 搜索历史下拉一屏列几条。与 searchLimit（存几条）刻意分开：存 20 但只列 15，
    // 用户常疑惑"明明存了 20 条却只看到 15 条"——现在这个数也可调。
    int searchHistoryMenuRows = 15;  // 搜索历史下拉行数（5..50）

    // 新标签位置："after_current" = 当前标签右侧；"end" = 最右。
    // 旧版本的 "right" 已归一成 "end"（它一直被标成"最右"，读入时纠正）。
    std::wstring tabNewPos = L"end";
    // 关闭窗口且标签数 > 1 时先确认。默认开：多标签是一次上下文，
    // 误按 Alt+F4 的代价比多一次点击高。最后一个标签关闭（= 退出）不弹。
    bool confirmCloseTabs = true;
    // 只有一个标签时是否隐藏标签栏。
    //
    // 【为什么需要】单标签下标签栏只显示一条"当前目录"，占掉 26px 高度却
    // 提供零信息 —— 垂直空间紧张（小屏/多窗口并排）时这 26px 很值钱。
    // Opus / Edge / Firefox 都有同款选项。默认关（与既有观感一致，不惊喜）。
    //
    // 【语义边界】"只有一个标签"指 tabSet_.Count() == 1；新建第二个标签时
    // 标签栏必须立刻出现，所以这个判断要进 Layout 的几何指纹（见 app_layout.cpp
    // 的 LayoutKey.showBits），否则切到单标签时不会重排。
    bool hideTabBarWhenSingle = false;

    // 标签宽度（逻辑像素）。默认 112–230 与旧行为一致；紧凑=80/160，
    // 宽松=140/320。固定标签不吃这两个值（恒为图标宽）。
    int tabMinWidth = 112;
    int tabMaxWidth = 230;
    // 拖拽标签到条边缘时的自动滚动步进周期（毫秒）。以前写死 30ms —— 标签多时
    // "滚太快冲过头 / 滚太慢够不着"是真实手感问题，注释里也承认是折中值。
    int tabDragScrollMs = 30;   // 标签拖拽自动滚动周期（10..200）

    // ---- 标签栏动效与拖出成窗（可关，也可调手感）----
    // 拖拽重排的让位动画时长。**0 = 关闭动画**（直接换位，与旧行为一致）：
    // 动效是偏好而不是正确性，必须留一条彻底关掉的路 —— 有人觉得滑一下更清楚，
    // 也有人觉得那是延迟。默认 140ms 与 Chromium 的标签让位同量级。
    int tabAnimMs = 140;
    // 被拖标签是否画成跟随光标的浮层（原槽留空）。关掉则退回"只有插入线"的旧观感。
    // 与 tabAnimMs 分开：一个是"拖动中"的反馈、一个是"松手后"的过渡，
    // 有人只想去掉其中一个，合成一个开关就表达不了。
    bool tabDragGhost = true;
    // 拖出成窗的垂直磁吸阈值（逻辑像素）。**0 = 关闭拖出成窗**，只保留右键菜单
    // 里的「移到新窗口」。默认 18dp：越过标签栏上下边缘这么多才算"想拖出去"，
    // 太小会被横向拖动时的手抖误判。
    int tabTearOffDy = 18;

    // 视图：8 档模式下标（0..7，见 app_commands.cpp 的 kViewModes）、排序与分组
    int viewMode = 3;
    int sortMode = 0;      // 0名称 1日期 2类型 3大小
    bool sortAscending = true;
    int groupBy = 0;       // 0无 1类型 2日期 3大小

    // 窗格显隐（每个标签启动时套用）
    bool showNavPane = true;
    bool showPreviewPane = true;

    // 导航窗格宽度（像素）。由拖动边界写入。
    // 【为什么这个值真的有意义】导航树现在是独立控件 INameSpaceTreeControl，
    // 宽度由布局算、由 SetWindowPos+WM_SIZE 落地，不再受 shell 框架的 150px 限制。
    // （旧的 EP_NavPane 裁剪法做不到这一点：探针 23 实测导航列恒为 150px。）
    // 上下限与默认值只在 layout_limits.h 定义一份 —— read/写盘/拖动范围都读它。
    int navPaneWidth = kNavPaneWidthDefault;

    // AI 面板宽度（像素）。由拖动边界写入，与 navPaneWidth 对称。
    //
    // 【为什么面板宽度必须可调】默认 420 是"一列消息 + 输入框"的宽度；而用户
    // 常拿面板贴文档/长路径读，420 明显不够（实测把 navPaneWidth 调宽后，
    // 面板里的代码块全是横向滚动条）。宽度写死在代码里时，唯一的办法是改分辨率。
    // 上下限见 layout_limits.h：下限 240 是气泡不折成两行字的极限。
    int aiPanelWidth = kAiPanelWidthDefault;

    // 导航树的根，一行一个：
    //   "clsid:679F85CB-0220-4080-B29B-5540CC05AAB6"  主页/快速访问
    //   "clsid:E88865EA-0E1C-4E20-9AA6-EDCD0212C87C"  图库
    //   "known:ComputerFolder" / "known:NetworkFolder" / "known:Profile" ...
    //   "path:D:\Projects"                             任意路径
    // 空 = 用内置默认（主页/图库/此电脑/网络，与 explorer 左栏一致）。
    std::vector<std::wstring> navPaneRoots;

    // 双栏：主窗格占视图区的百分比（20..80），默认左右均分。
    // 默认关闭；开启后每个标签按需创建第二个窗格，两栏各有自己的路径与历史。
    bool dualPane = false;
    int paneSplit = 50;
    // 双栏方向：false = 左右并排（默认），true = **上下堆叠**（垂直双栏）。
    //
    // 【为什么只是方向，不另开一套窗格模型】窗格槽位永远是两个（0/1），
    // 左右模式是 0=左、1=右；上下模式是 0=上、1=下。历史/路径/选中/视图档位
    // 全都挂在槽位上，方向只决定**这两个槽位怎么摆** —— 所以切方向不丢状态。
    // paneSplit 的含义随方向变：左右=左栏宽度百分比，上下=上栏高度百分比。
    bool paneVertical = false;
    // 双栏分割条上两条强调线的粗细（逻辑像素）：活动侧指示条与悬停/拖动加粗线。
    // 以前写死 S(2) —— 高 DPI 下太细、低分辨率下偏粗，是纯手感值。
    int splitMarkThick = 2;   // 分割条强调线粗细（1..6）

    // 窗口几何
    bool hasGeometry = false;
    int winX = 0, winY = 0, winW = 1100, winH = 760;
    bool maximized = false;
    // 会话快照落盘去抖（毫秒）：每次标签/路径变更重开这个计时器，连批操作只落一次盘。
    // 以前写死 2000 —— 它直接决定"崩溃前最多丢多久的标签状态"，是流畅性与数据安全的
    // 折中，关机慢或频繁批量开标签的用户需要能调。
    int sessionSaveDelayMs = 2000;  // 会话快照去抖（200..10000）

    // 会话 / 历史
    std::vector<std::wstring> recentPaths;   // 最近访问，front = 最新
    std::vector<std::wstring> sessionPaths;  // 上次关闭时的标签
    int sessionActive = 0;
    std::vector<std::wstring> sessionSelection;  // 关闭时活动标签的选中项（重启恢复用）
    // 与 sessionPaths 一一对应的每标签历史（可缺省：旧配置没有这一段）。
    std::vector<TabHistory> sessionHistory;
    // 双栏右窗格：与 sessionPaths 一一对应，空串表示该标签当时没有右窗格。
    std::vector<std::wstring> sessionPane2Paths;
    // 只记录有右窗格的标签，顺序与 sessionPane2Paths 中非空项一致。
    std::vector<TabHistory> sessionPane2History;
    // 命名会话：多条标签组（session.save/list/restore/delete 操作的是"当前那条"，
    // named.* 命令按名字操作这里的条目）。按名字覆盖保存。
    std::vector<NamedSession> namedSessions;
    std::vector<std::wstring> lockedPaths;   // 锁定标签（按路径持久化）
    std::vector<std::wstring> pinnedPaths;   // 固定标签（按路径持久化）
    std::vector<TabMeta> tabMeta;            // 标签改名/颜色（按路径持久化）
    std::vector<std::wstring> closedTabs;    // 关闭历史（可重开）

    // 状态栏段显隐（段号见 status_bar.h 的 SegId）。
    // 存储形态是**规范形**：升序、无重复 —— 写入与读盘都过 NormalizeStatusSegs，
    // 否则"写进去 [6,0,6]、重启变成 [0,6]"这种静默改写又会回来。
    std::vector<int> statusSegs{0, 1, 2, 3, 4, 5, 6};

    // ---- 搜索域（导航栏搜索框）----
    // 搜索范围档位。数值语义见 searchscope::Range（那里的枚举是唯一真源，
    // 这里只存数值）—— 0=当前目录本层 1=当前目录及子目录 2=此电脑 3=自定义路径。
    int searchRange = 1;
    // 自定义范围的路径（仅 searchRange==3 时有意义）。
    std::wstring searchRangePath;
    // 搜索历史：最近的在前。上限见 recentLimit 同组的 searchLimit（可调）。
    std::vector<std::wstring> searchHistory;

    // 快捷键覆盖：setting -> "Ctrl+Shift+T"（空串 = 显式禁用）
    std::vector<std::pair<std::wstring, std::wstring>> hotkeyOverrides;

    // ---- AI 操作通道 ----
    // 默认开启：这是"让外部程序驱动本窗口"的能力，人机等价（命令表是唯一真源）。
    // 开启后监听一个本地命名管道（名与令牌见 runtime\ai-pipe.txt / ai-token.txt，
    // 停服即删）。**支持热插拔**：config.set aiEnabled 会立即启停服务，无需重启
    // （见 App::SyncAiChannel）；关闭后不再监听任何连接。
    bool aiEnabled = true;
    // 是否允许 AI 执行 Risk::Danger 命令。关闭时这类命令一律拒绝（即使调用方
    // 带 approve=true）—— 相当于"人类保留最终授权"的硬开关。
    bool aiAllowDanger = false;
    // AI 通道的调用超时（毫秒）。以前硬编码在 ai_server.cpp 的 kUiWaitMs=10000：
    // 长命令（tool.dupfinder / diskusage 扫描大目录）在 UI 线程上跑满 10 秒后，
    // 客户端已经收到 error=timeout，而命令还在跑 —— 两端对"这次调用成没成"
    // 的看法不一致。超时该由使用者按自己的命令耗时决定，不写死在代码里。
    int aiTimeoutMs = 10000;
    // AI 请求泵的检查间隔（毫秒）。监听线程只把请求排队，真正执行在 UI 线程，
    // 这个定时器就是"多久看一眼队列"。以前写死 100 —— 它给每条命令加了最多
    // 100ms 的固定延迟，也是事件推送的节拍；低延迟自动化与省电诉求相反，故可调。
    int aiPumpIntervalMs = 100;  // AI 请求泵间隔（20..500）
    // AI 结果气泡（状态栏 chip 上方那块 peek）是否在终态时自动弹出。
    //
    // 【为什么需要关掉它】peek 的触发条件是"AI 跑到终态且面板收起"—— 它会在用户
    // 正看着文件列表时盖住一块视图，而用户可能只是让它后台跑个长任务。默认开
    // （这是面板收起后唯一的结果通知渠道，关掉就只能靠 chip 颜色变化发现）；
    // 但"我就是要它安静跑完"是明确诉求，必须给一条关掉的路。
    // 关闭后不影响 chip 的未读计数与颜色 —— 通知降级为"看得见但不打断"。
    bool aiPeekEnabled = true;

    // ---- 审计与后台作业（运维策略）----
    // 是否记录审计日志。默认开：审计是"人机等价"的取证面 —— 谁在什么时候调了
    // 哪条命令、参数是什么、成没成。关掉它，AI 侧的操作就没有可回溯的痕迹了。
    //
    // 【为什么仍要给开关】单用户长期跑自动化时，审计文件会持续增长（轮转大小
    // auditRotateKB 只能控制单文件），且每条命令都要拼一次 JSON。关掉它是
    // "我信自己的脚本、不要留痕"的显式选择，不是默认。
    bool auditEnabled = true;
    // 审计日志按大小轮转。此前 2MB 写死，高频 AI 调用很快就翻篇；合规场景常要
    // 延长保留。单位是 KB（界面友好），写盘时 <<10 换成字节。
    int auditRotateKB = 2048;      // 审计日志轮转大小（256..65536 KB）
    // 审计行里参数明文保留多少字符。是"能否看清一次调用传了什么"与隐私的权衡。
    int auditArgsTextLimit = 160;  // 审计参数明文上限（40..2000）

    // 后台机器同步任务的兜底等待上限（毫秒）。人侧有进度条与取消按钮不用它；
    // 机器侧没人盯屏，超时后客户端收到"没跑完"而作业仍在后台 —— 这个二分钟
    // 的折中值该由调用方按自己的作业耗时决定。与 aiTimeoutMs（管道超时）语义不同。
    int jobMachineTimeoutMs = 120000;  // 机器作业超时（1000..600000）
};

// 标签栏此刻是否应该显示。
//
// 【为什么是一个函数而不是多处写内联条件】这个判断有多个使用点（Layout 的
// 栏高、LayoutKey 的几何指纹、Layout 里的 ShowWindow、ChromeHeight 的两处
// 调用、启动期建树的高度）。各写一遍条件，只要有一处漏了"单标签隐藏"，
// 就会出现"栏藏起来了但高度还留着"或"高度收了但窗口没藏" —— 两种都是
// 看不见内容的空洞。语义只在这里定义一次，改规则只改这里。
//
// tabCount 由调用方给（Config 不知道标签集）：传 0 视为"还没建标签"，
// 此时按 showTab 走 —— 启动期不该因为"暂时 0 个标签"而把栏藏掉。
inline bool TabBarVisible(const Settings& s, int tabCount) {
    if (!s.showTab) return false;
    if (s.hideTabBarWhenSingle && tabCount == 1) return false;
    return true;
}

class Config {
public:
    static Config& Get();

    // 指定用户数据目录。必须在第一次 Load/Save 之前调用（main 启动时调用）。
    // 改目录会重置内存配置，保证不会把 A 目录的设置写进 B 目录。
    void SetProfileDir(const std::wstring& dir);
    void Load();
    // 全量写盘（调用点很少：改配置、关窗口）。
    //
    // 【为什么返回 bool】"改完了但没写进盘"是这个程序里最难察觉的一类失败：
    // 内存里的值是对的，界面也对，下次启动就变回去了 —— 用户看到的是
    // "设置自己改回去了"。两条失败路径（上次解析失败 → 拒绝覆盖；
    // 目录只读 → 写不进去）以前都只留在日志里，界面一个字都不说。
    // 调用方可以不理返回值（多数调用点是这个情形），但凡有用户界面在跟前的
    // 地方都应该读 lastSaveError() 把它说出来。
    // 写 data\settings.json（仅偏好）+ state\*.json（四份状态）。
    bool Save();
    // state\*.json 的读写入口（实现在 config_state_io.cpp，由 Load/Save 调用）。
    // 读：文件存在才覆盖 s_ 的状态字段；损坏则保留内存值并记日志（状态可丢，
    // 但不能因为一个坏文件把偏好也一起拖垮）。
    void LoadStateFiles();
    bool SaveStateFiles() const;
    // 最近一次 Save() 失败的原因（空 = 成功）。每次 Save() 开头清空。
    const std::wstring& lastSaveError() const { return saveError_; }
    // settings.json 存在但解析失败（内存里只有默认值，且已禁止写盘）。
    // 【为什么要公开】这个状态必须在用户动手改设置**之前**说出来 ——
    // 否则他改完十项、重启才发现一项都没生效，而这时已经无法判断是哪一步丢的。
    bool LoadFailed() const { return parseFailed_; }

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
    std::wstring RootDir() const;       // profile 根（默认 = exe 目录，见文件头）
    std::wstring Dir() const;           // 配置目录 = RootDir()\data
    std::wstring StateDir() const;      // = RootDir()\state（会话/历史状态）
    std::wstring LogsDir() const;       // = RootDir()\logs（审计/事件/对话）
    std::wstring CacheDir() const;      // = RootDir()\cache（WebView2）
    std::wstring RuntimeDir() const;    // = RootDir()\runtime（AI 发现文件）
    std::wstring SettingsPath() const;  // data\settings.json
    std::wstring BookmarksPath() const; // data\bookmarks.html
    std::wstring KeymapPath() const;    // data\keymap.json
    std::wstring MousePath() const;     // data\mouse.json
    std::wstring DataDir() const;       // 与 Dir() 相同：配置就放在 data\ 里
    // state\* 下四份状态文件的完整路径（供 config_io.cpp 的 Load/Save 使用）。
    std::wstring SessionPath() const;   // state\session.json
    std::wstring HistoryPath() const;   // state\history.json
    std::wstring TabsPath() const;      // state\tabs.json
    std::wstring WindowPath() const;    // state\window.json

    void AddRecent(const std::wstring& path);
    // 记一条搜索历史：去重、提到最前、超上限截断（最近的在前）。
    // 口径与 AddRecent 一致，只是上限更小（20 vs 30）—— 搜索词比路径更占地方。
    void AddSearchHistory(const std::wstring& query);
    void RememberSession(const std::vector<std::wstring>& paths, int active,
                         const std::vector<std::wstring>& selection = {},
                         const std::vector<TabHistory>& history = {},
                         const std::vector<std::wstring>& pane2Paths = {},
                         const std::vector<TabHistory>& pane2History = {});
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
    // profile 根（默认 = exe 目录）。各类子目录由它派生，换 profile 时整体切换。
    mutable std::wstring rootDir_;
    Settings s_;
    bool loaded_ = false;
    // 进程内启动开关，不随 SetProfileDir 重置（它在 main 每次启动只设一次），也不落盘。
    bool aiForceEnabled_ = false;
    // settings.json 存在但解析失败 → 禁止 Save 覆盖（否则用户手改打错一个逗号，
    // 下次任何一次 Save 都会把 recentPaths / sessionPaths / lockedPaths / closedTabs /
    // hotkeyOverrides 全部抹掉）。见 config.cpp Load() 的注释。
    bool parseFailed_ = false;
    // 上一次 Save() 失败的原因（空 = 成功）。见 Save() 的说明：
    // 这条错误必须能被界面读到，否则"改完下次启动又变回去"永远查不出来。
    std::wstring saveError_;
};
