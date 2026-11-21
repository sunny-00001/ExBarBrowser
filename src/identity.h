// 工程标识：品牌串、窗口类名、进程级资源名的唯一来源。
//
// 【为什么单独一个文件】独立前（2026-11-20 前）这些名字散在 9 个文件里，且三套写法并存：
//   L"ExBar"          —— 窗口标题、对话框标题（4 处）
//   L"ExBarBrowser"   —— 窗口类名（11 处）、命名管道、日志、互斥体
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

// 正式名：窗口类名、互斥体、管道、日志文件、环境变量。改它 = 改工程名。
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
constexpr wchar_t kBookmarkBarClass[] = L"ExBarBrowser_BookmarkBar";
constexpr wchar_t kPopupClass[] = L"ExBarBrowser_Popup";
constexpr wchar_t kPromptClass[] = L"ExBarBrowser_Prompt";
constexpr wchar_t kSeqHintClass[] = L"ExBarBrowser_SeqHint";
constexpr wchar_t kHotkeysClass[] = L"ExBarBrowser_Hotkeys";
constexpr wchar_t kReportClass[] = L"ExBarBrowser_Report";

// ---- 进程级资源名 ----
// 单实例互斥体（Local\ 前缀 = 每用户会话隔离）
constexpr wchar_t kInstanceMutex[] = L"Local\\ExBarBrowser.Instance";

// 诊断日志文件名（落在 %TEMP%）
constexpr wchar_t kLogFileName[] = L"ExBarBrowser.log";

// AI 通道命名管道前缀（后缀是主窗口句柄，见 ai_server.cpp）
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\ExBarBrowser_ai_";

// AI 服务标识（MCP initialize 回给客户端）
constexpr wchar_t kAiServiceName[] = L"exbarbrowser";

// 调试用环境变量：设为非空可绕过单实例（便于与被占用的实例并存）
constexpr wchar_t kMultiInstanceEnv[] = L"EXBARBROWSER_MULTI";
// 显式指定用户数据目录（相对/绝对均可）。测试用独立 profile，避免污染用户数据。
constexpr wchar_t kProfileEnv[] = L"EXBARBROWSER_PROFILE";

// WM_COPYDATA 单实例路径转发的魔数。
//
// 【为什么保留 0xEBB1】曾打算换成"无语义的固定值"，理由是它长得像 mod 期的玩笑
// （"EB"+"B1"）。但换值有真实成本：新旧版本窗口类名相同（ExBarBrowser_Main），
// 升级后旧进程还在跑时，新进程会找到它、发出带新魔数的消息、被旧进程静默拒绝 ——
// 用户看到的是"已有一个 ExBar 窗口未响应"，而那个进程其实好好的。
// 换句话说，换值买到的是"去掉一个玩笑"的审美收益，代价是一次真实的兼容断裂。
// 保留原值：它已经是既成契约。
constexpr ULONG_PTR kCopyDataMagic = 0xEBB1;

}  // namespace id
