// 工程标识：品牌串、窗口类名、进程级资源名的唯一来源。
//
// 【为什么单独一个文件】独立前（2026-11-20 前）这些名字散在 9 个文件里，且三套写法并存：
//   L"ExBar"          —— 窗口标题、对话框标题（4 处）
//   L"ExBarBrowser"   —— 窗口类名（11 处）、命名管道、日志
//   EXBAR_*           —— 书签属性（跨软件契约，必须保留）
// 独立后这些名字全部是"自用"，没有兼容义务，唯一的理由是"改起来要动 9 个文件"。
// 收到这里之后，未来改名只需动本文件。
//
// 【纪律】
//   · 品牌串只允许出现在本文件。其他文件出现 L"ExBar…" 字面量即违规（见 docs/NAMING.md B1）。
//   · EXBAR_* 书签属性**不在**本文件管理 —— 那是跨软件契约，见 bookmarks.h。
#pragma once

#include <windows.h>

namespace id {

// 正式名：窗口类名、管道、日志文件、环境变量。改它 = 改工程名。
constexpr wchar_t kBrand[] = L"ExBarBrowser";

// 显示名：窗口标题、对话框标题（用户可见）。允许与正式名不同。
constexpr wchar_t kBrandShort[] = L"ExBar";

// ---- 窗口类名 ----
// 规则 R12：窗口类总数 ≤3（Lister + Popup + Dialog）。现状仍多，逐个归并后收缩。
// Windows 惯例：类名只需进程内唯一，但必须不撞系统控件类名（Button/Edit/...），
// 所以一律带 kBrand 前缀。
constexpr wchar_t kMainClass[] = L"ExBarBrowser_Main";
constexpr wchar_t kViewAreaClass[] = L"ExBarBrowser_ViewArea";
constexpr wchar_t kTabViewClass[] = L"ExBarBrowser_TabView";
constexpr wchar_t kTabStripClass[] = L"ExBarBrowser_TabStrip";
constexpr wchar_t kNavBarClass[] = L"ExBarBrowser_NavBar";
constexpr wchar_t kStatusBarClass[] = L"ExBarBrowser_StatusBar";
// 状态栏右端的「图标大小」滑块（自绘，见 status_zoom_slider.h）。
// 独立于状态栏本体：状态栏的段模型是"文字 + 图标 + 气泡"，滑块要的是"按住拖"，
// 塞进段模型要给排版/命中/气泡三处开特例。
constexpr wchar_t kZoomSliderClass[] = L"ExBarBrowser_ZoomSlider";
constexpr wchar_t kBookmarkBarClass[] = L"ExBarBrowser_BookmarkBar";
// 布局边界条：夹在两块内容之间、可拖动改宽窄的真窗口（导航|文件、文件|AI、窗格|窗格）。
// 双栏那条沿用旧类名（外部脚本按它找窗口，是契约的一部分）；两条侧栏用新类名，
// 免得"按旧类名找分割条"的脚本突然命中三条、拿错条。
constexpr wchar_t kPaneSplitterClass[] = L"ExBarBrowser_PaneSplitter";
constexpr wchar_t kSideSplitterClass[] = L"ExBarBrowser_SideSplitter";
constexpr wchar_t kPopupClass[] = L"ExBarBrowser_Popup";
constexpr wchar_t kPromptClass[] = L"ExBarBrowser_Prompt";
// 导航窗格宿主：只用来承载 INameSpaceTreeControl（树由 shell 自己创建，
// 那个 SysTreeView32 是系统类名，不归我们登记）。隐藏/缩放/套主题都打在这个窗口上。
constexpr wchar_t kNavPaneHostClass[] = L"ExBarBrowser_NavPaneHost";
// AI 面板宿主：右侧一块 WebView2 的裁剪边界与父窗口。与 kNavPaneHostClass 对称
// （一个占最左、一个占最右），同样只承载第三方内容，不自绘。
constexpr wchar_t kAiPanelHostClass[] = L"ExBarBrowser_AiPanel";
// 状态栏 AI chip 上方的 peek 气泡：面板收起后的完成摘要 / 撤销 / 打开对话。
constexpr wchar_t kAiStatusPeekClass[] = L"ExBarBrowser_AiStatusPeek";
// 设置对话框（三栏：分类 / 表单 / 说明）。与主窗口是父子关系（owner=主窗口），
// 关掉主窗口时一并关闭。
constexpr wchar_t kSettingsClass[] = L"ExBarBrowser_Settings";
constexpr wchar_t kSeqHintClass[] = L"ExBarBrowser_SeqHint";
constexpr wchar_t kSeqOverlayClass[] = L"ExBarBrowser_SeqOverlay";
constexpr wchar_t kHotkeysClass[] = L"ExBarBrowser_Hotkeys";
constexpr wchar_t kReportClass[] = L"ExBarBrowser_Report";
// 长任务进度窗口（tool.dupfinder / tool.hash / tool.compare /
// tool.diskusage / tool.topn 在工作线程上跑时给人工看的进度条）。
constexpr wchar_t kProgressClass[] = L"ExBarBrowser_Progress";

// ---- 进程级资源名 ----
//
// 【不再有单实例互斥体】旧实现用 Local\ExBarBrowser.Instance 把程序锁成单实例，
// 第二个实例只能把路径转发给第一个再退出，"打开新窗口"实际上开不出来。
// 现在的契约是：
//   · 默认启动 —— 找得到已有窗口就把路径转发过去并激活它，找不到才新开进程；
//   · 显式多开 —— --new-window（进程内命令要走这条）或 EXBARBROWSER_MULTI 直接新开，
//     不查找、不转发、不阻断。
// 因此不存在任何"进程已存在 → 拒绝启动"的分支。

// 启动竞态标记（不是单实例锁）：后进者需要知道"是否已有更早实例正在建窗口"，
// 才决定要不要为转发短暂等待。首实例据此跳过 400ms 空等 —— 冷启动不该为一个
// 不存在的窗口付费。句柄只在进程生命周期内持有，所有实例退出后对象自动消失。
constexpr wchar_t kStartupMarker[] = L"Local\\ExBarBrowser.StartupMarker";

// 诊断日志文件名（落在 %TEMP%）
constexpr wchar_t kLogFileName[] = L"ExBarBrowser.log";

// AI 通道命名管道前缀（后缀是主窗口句柄，见 ai_server.cpp）
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\ExBarBrowser_ai_";

// AI 服务标识（MCP initialize 回给客户端）
constexpr wchar_t kAiServiceName[] = L"exbarbrowser";

// 显式多开：设非空即跳过"转发给已有窗口"，直接新起一个实例；
// 同时把 profile 根切到 profiles\multi（其下复刻 data/state/logs/cache/runtime，
// 见 config.h），避免两个实例抢同一份用户数据。
constexpr wchar_t kMultiInstanceEnv[] = L"EXBARBROWSER_MULTI";
// 显式多开的命令行形式（"在新窗口打开"命令用它启动子进程）。
// 与上面的环境变量不同：共享同一份用户数据（书签/设置），只是多一个窗口。
constexpr wchar_t kNewWindowArg[] = L"--new-window";
// 显式指定 profile 根（相对/绝对均可）。测试用独立 profile，避免污染用户数据；
// 其下复刻 data/state/logs/cache/runtime 整套。
constexpr wchar_t kProfileEnv[] = L"EXBARBROWSER_PROFILE";
// 启动时强制开启 AI 通道（覆盖 settings.json 的 aiEnabled，但不写回）。
// 值为 "0"/空 = 不强制；其它非空值 = 强制开启。配合 EXBARBROWSER_PROFILE 做隔离测试。
constexpr wchar_t kAiForceEnv[] = L"EXBARBROWSER_AI";
constexpr wchar_t kAiForceArg[] = L"--ai";
// 以 MCP 服务器身份启动：标准输入输出接到本进程的命名管道上，
// 让任何 MCP 客户端（Claude Desktop / VS Code / Cursor…）能直接驱动本程序。
// 见 src/mcp_stdio.h。
constexpr wchar_t kMcpStdioArg[] = L"--mcp-stdio";

// WM_COPYDATA 路径转发的魔数。
//
// 【为什么保留 0xEBB1】曾打算换成"无语义的固定值"，理由是它长得像 mod 期的玩笑
// （"EB"+"B1"）。但换值有真实成本：新旧版本窗口类名相同（ExBarBrowser_Main），
// 升级后旧进程还在跑时，新进程会找到它、发出带新魔数的消息、被旧进程静默拒绝 ——
// 用户看到的是"已有一个 ExBar 窗口未响应"，而那个进程其实好好的。
// 换句话说，换值买到的是"去掉一个玩笑"的审美收益，代价是一次真实的兼容断裂。
// 保留原值：它已经是既成契约。
constexpr ULONG_PTR kCopyDataMagic = 0xEBB1;

}  // namespace id
