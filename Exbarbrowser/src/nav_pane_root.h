// 导航树的根集：配置文本 <-> RootSpec <-> IShellItem。
//
// 【为什么与 nav_pane.h 分开】这里是**纯数据语义**：一行配置文本怎么解析、
// 已知文件夹名怎么查表、RootSpec 怎么变成 IShellItem。全部可在没有窗口、
// 没有消息循环的情况下单测（tests/nav_root_test.cpp）。nav_pane.cpp 那边是
// **控件语义**：建宿主、挂事件、套主题、改宽度 —— 两者改动理由完全不同，
// 混在一个文件里读的人要翻到中段才发现还有另一套语义。
#pragma once
#include "common.h"

#include <shobjidl.h>

#include <functional>
#include <vector>

namespace navpane {

// 导航树的一个根。用"种类 + 值"描述，便于配置化（一行文本一个根）。
struct RootSpec {
    enum class Kind {
        Clsid,   // shell 命名空间对象："shell:::{GUID}"
        Known,   // 已知文件夹：KNOWNFOLDERID 名（如 ComputerFolder）
        Path,    // 任意文件系统路径
    };
    Kind kind = Kind::Known;
    std::wstring value;   // 见 kind
    std::wstring label;   // 仅用于日志/排障，可为空
    // 分组序号（explorer 左栏的 0/1/2 三组，组间要画分隔线）。
    // 只有**运行时发现层**会设置它；配置解析出来的根恒为 0（同一组）。
    // 它不进配置文本 —— Format() 不写它，Parse() 也不读它。
    int group = 0;

    // 运行时发现层顺手采集的绝对 PIDL（WORD 对齐的字节串），**不进配置文本**。
    // 发现线程是独立 STA，IShellItem 不能跨线程用；PIDL 是纯字节，可以。
    // 主线程建树时优先用它 SHCreateItemFromIDList，省掉解析名那次 shell 绑定。
    // 空 = 配置来的 / 老路径，回退到 ItemFromRootSpec。
    std::vector<unsigned short> pidl;

    // explorer 左栏的那一套（运行时发现，见 nav_pane_explorer.cpp）
    static std::vector<RootSpec> Defaults();

    // 一行配置文本 -> RootSpec：
    //   "clsid:679F85CB-0220-4080-B29B-5540CC05AAB6"
    //   "known:ComputerFolder"       / "known:NetworkFolder"
    //   "path:C:\"
    static bool Parse(const std::wstring& text, RootSpec* out);
    // RootSpec -> 配置文本（与 Parse 互逆，用于写回配置）。
    static std::wstring Format(const RootSpec& spec);
};

// 已知文件夹名是否合法。配置里写错名字时 Parse 直接判无效（而不是静默
// 变成"桌面"），这里就是那条判据的唯一实现。
bool IsKnownFolderName(const std::wstring& name);

// 已知文件夹的**路径** -> 已知文件夹名（反向查表）。
//
// 用途：快速访问里那几项（桌面/文档/图片/音乐/视频…）本质就是已知文件夹，
// 用 Known 形式挂上去，图标与"点进去的行为"才和 explorer 一致；
// 用普通路径只会拿到通用文件夹图标。
// 路径对不上任何已知文件夹时返回 false（调用方改用 Path 形式）。
bool KnownFolderNameForPath(const std::wstring& path, std::wstring* name);

// RootSpec -> IShellItem。失败返回 nullptr（调用方决定要不要跳过这个根）。
// 调用方负责 Release。
IShellItem* ItemFromRootSpec(const RootSpec& spec);

// PIDL <-> 可跨线程保存的字节串。发现线程把 IShellItem 的绝对 PIDL 拷出来，
// UI 线程再重建 IShellItem；只搬字节，不跨 STA 传递 COM 接口指针。
std::vector<unsigned short> PidlBytes(PCIDLIST_ABSOLUTE pidl);
std::vector<unsigned short> PidlBytes(IShellItem* item);
IShellItem* ItemFromPidlBytes(const std::vector<unsigned short>& bytes);

// 任意路径/命名空间名 -> IShellItem（树里"定位到某项"用）。
// 与 ItemFromRootSpec 同一族语义：都是"外部/配置来的字符串 -> shell 对象"，
// 所以放在一起，而不是散落在控件实现里。调用方负责 Release。
IShellItem* ItemFromPath(const std::wstring& path);

// 把 shell 给的解析名归一化成"能被 SHParseDisplayName 接受"的形式。
//
// 【为什么需要它 —— 这是个真 bug，不是洁癖】
// SIGDN_DESKTOPABSOLUTEPARSING 对命名空间项返回**裸** `::{GUID}`，而裸形式对
// "主页"（679F85CB-…）会返回 E_INVALIDARG(0x80070057)，界面上就是
// "无法打开 ::{679F85CB-...}"。统一补 `shell:` 前缀后五种命名空间全部可解析
// （探针 49 实测），而文件系统路径原样返回、绝不能被套上前缀。
//
// 抽成纯函数是为了可单测：它是本次 bug 的核心判据，必须有用例钉住。
std::wstring NormalizeParsingName(const std::wstring& raw);

// ---- 根集发现的**异步预取** ----
//
// 【为什么需要异步】一次完整发现实测 461 ms（探针 72）：15 次
// SHCreateItemFromParsingName 绑定第三方网盘 shell 扩展 ≈279 ms，
// Home 命名空间枚举 19 项 + GetFileAttributes ≈178 ms。
// 这段同步跑在 UI 线程上时，消息循环不转 —— 窗口出不来、已有的树也不重绘。
//
// 所以：先用**内置三项**立刻把树画出来（0 ms），把发现放到工作线程，
// 算完回投 UI 线程替换根集。用户最多看到根集"变一次"，而不是启动头半秒整窗卡住。
//
// 三条约束（与 job::Service 同一套口径）：
//   1. 工作线程产出、UI 线程取走：结果只通过 PrefetchTake() 交付，交接有锁。
//   2. 只预取一次：重复调用是幂等的（已算过 / 正在算都直接返回）。
//   3. 不做取消：发现是一次性的、几百毫秒，退出时 PrefetchShutdown 里 join 掉。
//      **join 不可中断**（std::thread 不提供），所以 timeoutMs 只能表达
//      "等多久记一条告警"，不能表达"超时就放弃"——超时后仍会等它收工。
//
// 线程内会自己 CoInitializeEx（COM 单元是线程私有的，不能借用 UI 线程的）。
// 返回 false = 已启动过（幂等），不代表失败。
//
// onReady 在工作线程上、**发现结果已写进缓存之后**调用一次；
// 上层通常在这里 PostMessage 回 UI 线程（不要在工作线程上碰 UI）。
// 传空 = 不通知（调用方自己轮询 PrefetchTake，或根本只关心 Defaults() 的兜底）。
bool PrefetchDefaultsAsync(std::function<void()> onReady = {});

// 取走预取结果。返回 true = 取到了（roots 已填好，之后缓存即用这份）。
// 没算完/没启动过返回 false。只能在 UI 线程调用。
bool PrefetchTake(std::vector<RootSpec>* roots);

// 等预取线程真正退出（进程退出前调一次）。timeoutMs=0 表示无限等。
// 与 job::Service::Shutdown 同一个理由：不 join 会在工作线程里访问已析构对象。
void PrefetchShutdown(unsigned timeoutMs);

// ---- 运行期根集重发现 ----
//
// explorer 左栏是**配置驱动**的：用户在 explorer 里取消固定某个命名空间/
// 快捷访问项，改的是注册表与 shell 状态，不会通知我们替换 RootSpec 缓存。
// 只靠启动那次 Prefetch 永远看不到这种变化。
//
// RefreshDefaultsAsync 重新跑一遍发现并发布新缓存；调用方在 onReady 里
// 回投 UI 线程拿 PrefetchTake() 换根。重复调用会合并成一次在跑的发现。
bool RefreshDefaultsAsync(std::function<void()> onReady = {});

// 监听三类 explorer 侧边栏配置来源：
//   · HKCU\...\Explorer\Desktop 子树（第三方命名空间项增删/改名）；
//   · HKCU\...\Explorer\Advanced 的值（Folder Options 的显示开关）；
//   · %APPDATA%\Microsoft\Windows\Recent\AutomaticDestinations 目录
//     （主页/快速访问的 Pin/Unpin，这里不在注册表里）。
// 变化后触发 RefreshDefaultsAsync。必须在退出前 Stop。
bool StartRootChangeWatch(std::function<void()> onChanged);
void StopRootChangeWatch();

}  // namespace navpane