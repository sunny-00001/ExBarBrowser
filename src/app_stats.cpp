// 状态栏取数（App 的编排分片）：枚举当前视图的分类/选中/容量，换算成 StatusModel。
//
// 这一块的复杂度全在"什么时候取"（节流）和"取不到时显示什么"，与布局/标签无关，
// 所以单独一片；改这里只有一个理由：状态栏显示什么数。
#include "app.h"

// ---------------------------------------------------------------- 状态栏取数
void App::RefreshStats() {
    if (!Config::Get().s().showStatus) return;
    Tab* t = Active();
    if (!t) return;

    static int tick = 0;
    static std::wstring lastPath;
    ++tick;

    const std::wstring path = t->path();
    const bool pathChanged = (path != lastPath);
    lastPath = path;

    // 节流：导航变化立即取数；否则后台/大目录降频，避免每 800ms 全量枚举
    if (!pathChanged) {
        HWND fg = GetForegroundWindow();
        const bool foreground = fg && (fg == hwnd_ || GetAncestor(fg, GA_ROOT) == hwnd_);
        if (!foreground && (tick % 5) != 0) return;
        if (t->items > 3000 && (tick % 4) != 0) return;
        if (t->items > 800 && (tick % 2) != 0) return;
    }

    IShellView* view = t->view->CurrentView();
    ViewStats stats;
    bool ok = false;
    if (view) ok = CollectViewStats(view, &stats, &t->split);

    if (ok && stats.total) t->items = stats.total;

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
        m.text[Seg_Path] = t->display.empty() ? t->path() : t->display;
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
        m.tip[Seg_Path] = t->path();
        m.tip[Seg_Size] = L"选中对象容量（文件夹为 shell 聚合值）";
        m.tip[Seg_Disk] = stats.containerPath;
    } else {
        m.text[Seg_Chk] = t->items ? CountText(t->items) : L"";
        m.text[Seg_Path] = t->display.empty() ? t->path() : t->display;
        m.tip[Seg_Path] = t->path();
    }
    if (view) view->Release();

    SHELLSTATE ss{};
    SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    m.hiddenState = !ss.fShowAllObjects ? 2 : (!ss.fShowSuperHidden ? 1 : 0);
    m.text[Seg_Eye] = m.hiddenState == 0 ? L"显示隐藏" : (m.hiddenState == 1 ? L"隐系统" : L"隐全部");
    m.tip[Seg_Eye] = L"隐藏文件三态：全显 → 隐系统 → 隐全部（点击切换）";
    m.text[Seg_Type] = L"类型";
    m.tip[Seg_Type] = L"选中项类型";

    statusBar_.SetModel(m);

    // 低音量诊断：路径/计数变化时记一行（排查状态栏取数用）
    static std::wstring lastSig;
    std::wstring sig = t->path() + L"|" + m.text[Seg_Fld] + L"|" + m.text[Seg_File] + L"|" +
                       m.text[Seg_Chk] + L"|" + m.text[Seg_Date] + L"|" + m.text[Seg_Size] +
                       L"|" + m.text[Seg_Disk];
    if (sig != lastSig) {
        lastSig = sig;
        LogDbg(L"[status] %s | %s | %s | %s | %s | %s", m.text[Seg_Fld].c_str(),
               m.text[Seg_File].c_str(), m.text[Seg_Chk].c_str(), m.text[Seg_Date].c_str(),
               m.text[Seg_Size].c_str(), m.text[Seg_Disk].c_str());
    }
}
