// 视图相关的两个**反复弹出**的菜单：选择详细信息列 / 图标大小。
//
// 【为什么从 app_commands_files.cpp 拆出来】它们与那里其余内容的关系只是
// "都在视图域"，而真正的读者不同：
//   · app_commands_files.cpp 装的是 file.* / view.* 的**命令处理器**；
//   · 这两个函数是**菜单循环**（一个反复弹到用户选"完成"，一个列 8 档图标尺寸），
//     与 nav_bar_view_menu.cpp 的"视图/排序/分组"菜单是同一族（视图属性的选择器）。
// 混在命令处理器里时，改菜单要翻过一千行处理器代码；文件也已顶到 500 行闸门。
//
// 【拆分的额外好处】菜单循环的键位标注（`.Cmd(CmdId::ViewZoomSet, "48")`）与
// nav_bar_view_menu 的（`.Cmd(CmdId::ViewMode, "3")`）现在挨得近，两处的
// "带参身份 → 快捷键"写法可以并排看，不容易一处写对、另一处写成不带参。
#include "app.h"

#include "menu.h"            // ui::Menu（行构建与自动快捷键列）
#include "shell_columns.h"   // 列枚举（与 view.columns.* 同一份实现）

void App::ShowColumnMenu(POINT at) {
    // 每次重开菜单都重新读列状态：上一次勾选已经通过 view.columns.set 落地，
    // 这里若沿用旧快照，第二次勾选会把前一次改动覆盖掉。
    for (;;) {
        std::vector<shellcolumns::ColumnDesc> cols;
        if (IShellView* v = ActiveView()) {
            const bool ok = shellcolumns::List(v, &cols);
            v->Release();
            if (!ok || cols.empty()) {
                CommandBus::Get().NoteFailure(L"当前视图不支持详情列管理（需详细信息视图）",
                                         FailKind::NotSupported);
                return;
            }
        } else {
            CommandBus::Get().NoteFailure(L"没有活动文件视图，无法选择详情列",
                                         FailKind::NotSupported);
            return;
        }

        ui::Menu menu;   // 列勾选不挂 Cmd：参数是列 id 数组，键位表无按列绑的键
        for (size_t i = 0; i < cols.size(); ++i)
            menu.AddChecked(cols[i].name, cols[i].visible, static_cast<int>(i),
                            ico::IconId::DetailsView, !cols[i].canHide);
        menu.Separator();
        menu.Add(L"完成", -2, ico::IconId::Check);   // 纯 UI：关本菜单，无命令

        ui::MenuOptions opt;
        opt.maxRows = 30;
        opt.toggleKey = L"view.columns";   // 同一实例内防重复打开
        const int pick = menu.Show(hwnd_, theme_, at, opt);
        if (pick == -2 || pick < 0 || pick >= static_cast<int>(cols.size())) break;

        std::vector<std::wstring> visible;
        for (size_t i = 0; i < cols.size(); ++i) {
            const bool shown = (static_cast<int>(i) == pick) ? !cols[i].visible : cols[i].visible;
            if (shown) visible.push_back(cols[i].id);
        }
        json::Value args = json::Value::MakeObject();
        json::Value arr = json::Value::MakeArray();
        for (const std::wstring& id : visible) arr.arr.push_back(json::Value::MakeStr(id));
        args.Set(L"visible", std::move(arr));
        CommandBus::Get().Invoke(CmdId::ViewColumnsSet, Args::FromJson(args), Actor::Ui());
    }
}

void App::ShowZoomMenu(POINT at) {
    static constexpr int kSizes[] = {16, 32, 48, 64, 96, 128, 192, 256};
    ShellView* host = ActiveShellViewHost();
    if (!host) {
        CommandBus::Get().NoteFailure(L"没有活动文件视图，无法调整图标大小");
        return;
    }
    int fvm = 0, current = 0;
    if (!host->GetViewMode(&fvm, &current)) {
        CommandBus::Get().NoteFailure(L"读不到当前图标大小");
        return;
    }

    ui::Menu menu;
    int initial = -1;
    for (size_t i = 0; i < _countof(kSizes); ++i) {
        const int row = menu.AddChecked(std::to_wstring(kSizes[i]) + L" 像素",
                                        kSizes[i] == current, static_cast<int>(i),
                                        ico::IconId::GridView);
        // 像素当参数（每档一条 view.zoom.set:N）；只传身份会让 8 行全显示同一个键。
        menu.Cmd(row, CmdId::ViewZoomSet, std::to_wstring(kSizes[i]));
        if (kSizes[i] == current) initial = row;
    }
    ui::MenuOptions opt;
    opt.toggleKey = L"view.zoom";
    opt.initialRow = initial;
    const int pick = menu.Show(hwnd_, theme_, at, opt);
    if (pick < 0 || pick >= static_cast<int>(_countof(kSizes))) return;

    json::Value args = json::Value::MakeObject();
    args.Set(L"iconPx", json::Value::MakeInt(kSizes[pick]));
    CommandBus::Get().Invoke(CmdId::ViewZoomSet, Args::FromJson(args), Actor::Ui());
}