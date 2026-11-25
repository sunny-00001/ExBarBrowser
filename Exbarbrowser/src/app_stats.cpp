// 状态栏取数（App 的编排分片）：枚举当前视图的分类/选中/容量，换算成 StatusModel。
//
// 这一块的复杂度全在"什么时候取"（节流）和"取不到时显示什么"，与布局/标签无关，
// 所以单独一片；改这里只有一个理由：状态栏显示什么数。
#include "app.h"

#include "icon_resolve.h"

// ---------------------------------------------------------------- 状态栏取数
void App::MarkStatsDirty() { statsDirty_ = true; }

void App::RefreshStats() {
    if (!Config::Get().s().showStatus) return;
    Tab* t = Active();
    if (!t) return;
    Pane* p = ActivePane();
    if (!p) return;

    unsigned& tick = statsTick_;
    ++tick;

    // 显式命令会先 MarkStatsDirty() 再同步调 RefreshStats()。这里把脏标记取成
    // 局部量：本帧无论是否真的走到全量取数，都视为“这一次显式请求已被消费”。
    // 旧写法让脏标记留在成员里，而 800ms 定时器又被节流提前 return，于是它
    // 永远为 true，轻量短路一次都进不去。
    const bool forced = statsDirty_;

    const std::wstring path = p->path();
    const bool pathChanged = (path != statsLastPath_);

    IShellView* view = p->view->CurrentView();
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    const int hidden = !ss.fShowAllObjects ? 2 : (!ss.fShowSuperHidden ? 1 : 0);

    // P0-2：每个 800ms 空闲帧先只取 total + 选中数（两次 COM 调用）。
    // 没变化就直接返回，不再每拍扫 500 项选中属性、容器属性、排序方向和磁盘。
    // 显式命令会先 MarkStatsDirty()，所以“选中数没变但选了别的文件”也能刷新。
    if (view && !forced) {
        ViewStats light;
        if (CollectViewStats(view, &light, nullptr, {}, true, true)) {
            const bool same = !pathChanged && light.total == statsLastTotal_ &&
                              light.selCount == statsLastSelCount_ && hidden == statsLastHidden_ &&
                              view == statsLastView_ && p->display == statsLastDisplay_;
            if (same) {
                view->Release();
                return;
            }
        }
    }

    // 【节流：什么时候允许"逐项分类"】探针 82 实测 System32（5003 项）：
    //   只数总数 8.2 ms，逐项分类 85.7 ms —— 差 10 倍，全在 UI 线程。
    //
    // 旧口径是"路径一变就无条件全量分类"。导航本来就是路径变化最频繁的时刻，
    // 于是每次进大目录都卡 86 ms。现在的口径：
    //   · 路径刚变（导航）-> **不做**分类，只让 CollectViewStats 拿总量 + 选中集
    //     （这两项便宜），分类框显示上次缓存或留空
    //   · 稳定下来（同一个路径的第 2 次调用起）才允许分类，且受规模/前台节流
    // 这样"导航那一刻"的 86 ms 就没了，用户看到的状态栏数字会在 ~800ms 后补齐 ——
    // 状态栏数字不是第一眼必需，而导航卡顿是。
    if (!pathChanged) {
        HWND fg = GetForegroundWindow();
        const bool foreground = fg && (fg == hwnd_ || GetAncestor(fg, GA_ROOT) == hwnd_);
        if (!forced && !foreground && (tick % 5) != 0) { if (view) view->Release(); return; }
        if (!forced && p->items > 3000 && (tick % 4) != 0) { if (view) view->Release(); return; }
        if (!forced && p->items > 800 && (tick % 2) != 0) { if (view) view->Release(); return; }
    }

    ViewStats stats;
    bool ok = false;
    // 允许分类的判据：路径已稳定（不是刚导航过来的那一帧）且规模不过大。
    // allowSplit=false 时 CollectViewStats 只取总量/选中集，跳过逐项分类。
    const bool allowSplit = !pathChanged && p->items <= kStatsSplitCap;
    if (view) ok = CollectViewStats(view, &stats, &p->split, path, allowSplit);

    if (ok && stats.total) p->items = stats.total;
    if (ok) {
        // 全量取数成功才算“这一帧已经同步过”。脏标记已在帧首取成 forced，
        // 显式命令这一帧一定跳过节流、走到这里；定时器帧没变化则被上面的
        // 轻量判据提前返回。
        statsLastPath_ = path;
        statsLastDisplay_ = p->display;
        statsLastTotal_ = stats.total;
        statsLastSelCount_ = stats.selCount;
        statsLastHidden_ = hidden;
        statsLastView_ = view;
        statsDirty_ = false;
    }

    StatusModel m;
    if (ok) {
        if (stats.splitKnown) {
            m.text[Seg_Fld] = L"文件夹 " + std::to_wstring(stats.selFolders) + L"/" +
                              std::to_wstring(stats.folders);
            m.text[Seg_File] = L"文件 " + std::to_wstring(stats.selFiles) + L"/" +
                               std::to_wstring(stats.files);
        } else {
            // 超过 kSplitCap（8000）或枚举中断：不要再显示 "文件夹 0/0"，
            // 与旁边"已选 x/N"自相矛盾。回退成"N 个对象"。
            m.text[Seg_Fld] = std::to_wstring(stats.total) + L" 个对象";
            m.text[Seg_File] = L"";
        }
        m.text[Seg_Chk] = L"已选 " + std::to_wstring(stats.selCount) + L"/" +
                          std::to_wstring(stats.total);
        m.text[Seg_Date] = stats.hasModified ? FormatDate(stats.modified) : L"—";
        if (stats.dateSort > 0) m.text[Seg_Date] += L" ↑";
        if (stats.dateSort < 0) m.text[Seg_Date] += L" ↓";
        m.text[Seg_Path] = p->display.empty() ? p->path() : p->display;
        m.tip[Seg_Path] = p->path();
        // 路径段的**真实系统图标**：显示"你正在哪"的不只是文字，还有那一枚图标。
        // ico::Cached 是进程级缓存（句柄由它持有），这里只借用不销毁。
        m.pathIcon = p->path().empty() ? nullptr : ico::Cached(p->path(), true);
        m.text[Seg_Size] = stats.selBytesKnown ? L"选中 " + FormatSize(stats.selBytes) : L"";
        // 选中集超过 500 项时只统计了前 500 项：标出来，别让用户以为总量就这么多。
        if (stats.selTruncated && !m.text[Seg_Size].empty()) m.text[Seg_Size] += L"（前 500 项）";
        if (stats.hasDisk) {
            m.text[Seg_Disk] = L"磁盘 " + FormatSize(stats.diskFree) + L" 可用 / " +
                               FormatSize(stats.diskTotal);
        } else if (stats.containerBytesKnown) {
            m.text[Seg_Disk] = L"文件夹 " + FormatSize(stats.containerBytes);
        }

        m.tip[Seg_Fld] = L"文件夹：选中 " + std::to_wstring(stats.selFolders) + L" / 总数 " +
                         std::to_wstring(stats.folders);
        m.tip[Seg_File] = L"文件：选中 " + std::to_wstring(stats.selFiles) + L" / 总数 " +
                          std::to_wstring(stats.files);
        m.tip[Seg_Chk] = L"已选 " + std::to_wstring(stats.selCount) + L" 项，共 " +
                         std::to_wstring(stats.total) + L" 项";
        m.tip[Seg_Date] = L"修改日期（有选中取最新，否则取当前文件夹）";
        m.tip[Seg_Path] = p->path();
        m.tip[Seg_Size] = L"选中对象容量（文件夹为 shell 聚合值）";
        m.tip[Seg_Disk] = stats.containerPath;
    } else {
        m.text[Seg_Chk] = p->items ? CountText(p->items) : L"";
        m.text[Seg_Path] = p->display.empty() ? p->path() : p->display;
        m.tip[Seg_Path] = p->path();
        m.pathIcon = p->path().empty() ? nullptr : ico::Cached(p->path(), true);
    }
    if (view) view->Release();

    m.hiddenState = hidden;
    // 隐藏三态的文字**统一是当前状态**（全显 / 隐系统 / 隐全部）。
    // 旧口径把"显示隐藏"（动作）与"隐系统"（状态）混在一栏里 —— 同一个段里两种
    // 语义，读的人得先猜这句是"现在怎样"还是"点了会怎样"。动作说明放 tooltip。
    m.text[Seg_Eye] = m.hiddenState == 0 ? L"全显" : (m.hiddenState == 1 ? L"隐系统" : L"隐全部");
    m.tip[Seg_Eye] = L"隐藏文件三态：全显 → 隐系统 → 隐全部（点击切换）";

    // 类型段：显示选中项的扩展名；无选中/全是文件夹时**不显示**（不画光杆图标）。
    if (stats.selExtKinds == 1) {
        m.text[Seg_Type] = stats.selExt;
        m.tip[Seg_Type] = L"选中项类型：" + stats.selExt;
    } else if (stats.selExtKinds > 1) {
        m.text[Seg_Type] = L"多种类型";
        m.tip[Seg_Type] = L"选中项包含 " + std::to_wstring(stats.selExtKinds) + L" 种类型";
    } else {
        m.text[Seg_Type] = L"";
        m.tip[Seg_Type] = L"";
    }

    // AI chip 是常驻段，但它的数据源不是当前目录统计。RefreshStats 每次重建
    // StatusModel 时必须把上一次异步写入的 chip 文本带回，否则 800ms 后会被
    // 构造函数里的默认值"AI · 空闲"悄悄盖掉（运行中看着像没事发生）。
    m.text[Seg_Ai] = statusBar_.model().text[Seg_Ai];
    m.tip[Seg_Ai] = statusBar_.model().tip[Seg_Ai];
    statusBar_.SetModel(m);

    // 低音量诊断：路径/计数变化时记一行（排查状态栏取数用）
    static std::wstring lastSig;
    std::wstring sig = p->path() + L"|" + m.text[Seg_Fld] + L"|" + m.text[Seg_File] + L"|" +
                       m.text[Seg_Chk] + L"|" + m.text[Seg_Date] + L"|" + m.text[Seg_Size] +
                       L"|" + m.text[Seg_Disk];
    if (sig != lastSig) {
        lastSig = sig;
        LogDbg(L"[status] %s | %s | %s | %s | %s | %s", m.text[Seg_Fld].c_str(),
               m.text[Seg_File].c_str(), m.text[Seg_Chk].c_str(), m.text[Seg_Date].c_str(),
               m.text[Seg_Size].c_str(), m.text[Seg_Disk].c_str());
    }
}
