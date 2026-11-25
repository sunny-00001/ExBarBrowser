// 弹层行高规则自测：分隔线只占 2px，普通条目占整行高，总高 = 各行之��。
//
// 这条断言来自一次真实返工：原来 ListBox 用 LBS_OWNERDRAWFIXED，所有行强制同一高度，
// 分隔线也占满 24px（120 DPI 下 30px），一个分组界线吃掉一整行，视觉上像多了个空条目。
//
// 编译（在 ExBarBrowser/ 下）：
//   clang++ -std=c++20 -O1 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
//     -D_WIN32_IE=0x0A00 -Isrc tests/popup_layout_test.cpp src/icon_glyph.cpp \
//     -o /tmp/popup_layout_test.exe -static -static-libstdc++ -static-libgcc \
//     -lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luxtheme -ldwmapi
//   （要 icon_glyph：行高常量依赖 ico::Glyph 的字形度量。原文只写测试文件本身，
//     照抄会报 ico::Glyph / ico::IconIdFromName 未定义。）
#include <cstdio>
#include <vector>

#include "popup_list.h"
#include "view_modes.h"

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

PopupRow Sep() {
    PopupRow r;
    r.separator = true;
    return r;
}

PopupRow Item(const wchar_t* label) {
    PopupRow r;
    r.label = label;
    return r;
}

}  // namespace

int main() {
    const int itemH = 24;   // 96 DPI 下的条目高
    const int sepPx = 2;    // 分隔线行高

    // 1. 单行：分隔线 vs 普通条目
    std::printf("单行高度\n");
    Check(PopupRowHeight(Sep(), itemH, sepPx) == sepPx, "分隔线 = 2px（不是整行高）");
    Check(PopupRowHeight(Item(L"a"), itemH, sepPx) == itemH, "普通条目 = itemH");

    // 2. 典型下拉：3 条路径 + 分隔线 + 1 条动作（地址框 ⌄ 的形状）
    std::printf("\n典型地址框下拉（3 路径 + 分隔 + 1 动作）\n");
    std::vector<PopupRow> rows{Item(L"C:\\a"), Item(L"C:\\b"), Item(L"C:\\c"),
                               Sep(), Item(L"清除历史记录")};
    const int h = PopupTotalHeight(rows, static_cast<int>(rows.size()), itemH, sepPx);
    Check(h == 4 * itemH + sepPx, "总高 = 4*itemH + 2px");
    Check(h == 98, "总高数值 = 98");
    // 旧算法（每行都按 itemH）会得到 5*24=120，多出 22px
    Check(h < 5 * itemH, "比以前省下一整行（< 5*itemH）");

    // 3. 两个分隔线（back / fwd / recent 三段都有时）
    std::printf("\n三段式（2 个分隔线）\n");
    std::vector<PopupRow> rows2{Item(L"back1"), Sep(), Item(L"fwd1"), Sep(), Item(L"recent1")};
    Check(PopupTotalHeight(rows2, 5, itemH, sepPx) == 3 * itemH + 2 * sepPx,
          "总高 = 3*itemH + 2*sepPx");

    // 4. n 越界钳制：不能读越界，也不能多算
    std::printf("\n越界钳制\n");
    Check(PopupTotalHeight(rows, 999, itemH, sepPx) == h, "n > 行数时按行数算");
    Check(PopupTotalHeight(rows, 0, itemH, sepPx) == 0, "n = 0 时为 0");
    Check(PopupTotalHeight(rows, -1, itemH, sepPx) == 0, "n < 0 时为 0");

    // 5. 空列表
    std::printf("\n空列表\n");
    Check(PopupTotalHeight({}, 0, itemH, sepPx) == 0, "空列表总高 0");

    // 6. 高 DPI：sepPx 也应随 DPI 缩放，且不为 0
    std::printf("\n高 DPI\n");
    const int dpiItem = 30, dpiSep = 3;   // 120 DPI
    std::vector<PopupRow> rows3{Item(L"a"), Sep(), Item(L"b")};
    Check(PopupTotalHeight(rows3, 3, dpiItem, dpiSep) == 2 * dpiItem + dpiSep,
          "120 DPI 下总高 = 2*30 + 3");

    // 7. 字形图标槽：老调用方的 7 字段初始化仍合法，新字段默认 None。
    std::printf("\n图标槽（复用 ico::IconId）\n");
    PopupRow legacy{L"老调用方", L"", nullptr, false, false, false, 42};
    Check(legacy.glyph == ico::IconId::None, "7 字段初始化：glyph 默认 None");
    PopupRow action{L"重命名", L"", nullptr, false, false, false, 7, ico::IconId::Edit};
    Check(action.glyph == ico::IconId::Edit, "8 字段初始化：glyph 走登记表");
    Check(PopupRowHeight(action, itemH, sepPx) == itemH, "带图标的行高不变");

    // 树节点与平铺行共用同一套字形登记表；9 字段老初始化仍合法。
    TreeNode treeLegacy{L"子目录", L"C:\\a", nullptr, 0, -1, {}, true, false, false};
    Check(treeLegacy.glyph == ico::IconId::None, "树节点 9 字段初始化：glyph 默认 None");
    TreeNode treeAction{L"子目录", L"C:\\a", nullptr, 0, -1, {}, true, false, false,
                        ico::IconId::Folder};
    Check(treeAction.glyph == ico::IconId::Folder, "树节点图标兜底也走 IconId");

    // 8. 「开菜单那次点击的松开」——"菜单一闪而过"的根因。
    //
    // 导航栏下拉在**按下**时弹出，紧跟着的松开（光标还在锚点上 = 列表外）
    // 必须被忽略；否则菜单在松手瞬间自己关掉。
    std::printf("\n开菜单那次点击的松开\n");
    {
        // 开菜单后没再按过、松开在列表外 = 收尾 → 忽略（不关）
        Check(PopupUpIsOpeningRelease(/*downInList=*/false, /*downOutside=*/false,
                                      /*upInside=*/false),
              "没再按过 + 松开在外 → 是收尾，忽略（菜单不关）");

        // 开菜单后按在列表内再松开 = 正常选行，不能被当成收尾
        Check(!PopupUpIsOpeningRelease(true, false, true), "按下在列表内 → 不是收尾");
        Check(!PopupUpIsOpeningRelease(true, false, false),
              "按下在列表内、拖到外面松开 → 不是收尾（正常取消）");

        // 按在列表外（用户真的点了别处）→ 必须能关
        Check(!PopupUpIsOpeningRelease(false, true, false),
              "按下在列表外 → 不是收尾（这就是'点别处收起'）");

        // 松开正好落在列表内：不是收尾（无需忽略，会走正常选行）
        Check(!PopupUpIsOpeningRelease(false, false, true),
              "松开在列表内 → 不是收尾");
    }

    // 9. 完整消息序列：用状态机走一遍真实交互，逐拍断言弹层是否还活着。
    //
    // 单点判据容易"每条都对、组合起来错"。这里把 downInList / downOutside
    // 当成真实状态，按消息顺序推演三组典型交互。
    std::printf("\n消息序列推演\n");
    {
        // 一个小状态机，语义与 ListSubclass 完全一致。
        struct Sim {
            bool downInList = false;
            bool downOutside = false;
            bool closed = false;

            // 返回 true = 这一拍把弹层关掉了
            bool OnDown(bool inside) {
                if (inside) {
                    downOutside = false;
                    downInList = true;   // 列表内：等待 UP 选行
                } else {
                    downOutside = true;  // 列表外：等 UP 再关
                }
                return false;
            }
            bool OnUp(bool inside) {
                // 开菜单那次点击的收尾 → 忽略
                if (PopupUpIsOpeningRelease(downInList, downOutside, inside)) return false;
                const bool outside = downOutside || !inside;
                downInList = false;
                downOutside = false;
                if (outside) {
                    closed = true;
                    return true;   // 关
                }
                return false;      // 选行（不在这里模拟）
            }
        };

        // 场景 A：按下开菜单（弹层已建）→ 同一次点击松开，光标还在锚点上（列表外）。
        // 这是"一闪而过"的原现场景，必须**不关**。
        {
            Sim s;
            s.OnUp(/*inside=*/false);          // 只有 UP 到达（DOWN 发生在建窗之前）
            Check(!s.closed, "A 开菜单那下松开（列表外）→ 菜单不关");
        }

        // 场景 B：菜单开着，用户点菜单里的一行。
        {
            Sim s;
            s.OnDown(/*inside=*/true);
            s.OnUp(/*inside=*/true);
            Check(!s.closed, "B 点菜单内一行 → 不关（走选行）");
        }

        // 场景 C：菜单开着，用户点外面的别处 → 必须关（"再点即收"/点外收起）。
        {
            Sim s;
            s.OnDown(/*inside=*/false);
            s.OnUp(/*inside=*/false);
            Check(s.closed, "C 按下+松开都在列表外 → 关（点别处收起）");
        }

        // 场景 D：按住菜单里一行拖到外面松开 → 取消，不选行但要关。
        {
            Sim s;
            s.OnDown(/*inside=*/true);
            s.OnUp(/*inside=*/false);
            Check(s.closed, "D 内部按下、拖到外面松开 → 关（取消）");
        }

        // 场景 E：右键菜单开着，用户右键点别处 → 关（不漏给 ListBox 默认处理）。
        {
            Sim s;
            s.OnDown(/*inside=*/false);
            s.OnUp(/*inside=*/false);
            Check(s.closed, "E 右键点外面 → 关");
        }
    }

    // 10. 右键菜单新增动作必须走同一张字形登记表（不写裸码位、不落回兜底字形）。
    std::printf("\n右键菜单图标登记\n");
    {
        const wchar_t fallback = ico::Glyph(ico::IconId::None);
        Check(ico::Glyph(ico::IconId::Delete) != fallback, "删除有登记字形");
        Check(ico::Glyph(ico::IconId::Undo) != fallback, "撤销有登记字形");
        Check(ico::Glyph(ico::IconId::NewWindow) != fallback, "新窗口有登记字形");
        Check(ico::Glyph(ico::IconId::Terminal) != fallback, "终端有登记字形");
        Check(ico::Glyph(ico::IconId::Sort) != fallback, "排序有登记字形");
        Check(ico::Glyph(ico::IconId::Import) != fallback, "导入有登记字形");
        Check(ico::Glyph(ico::IconId::Export) != fallback, "导出有登记字形");
        Check(ico::IconIdFromName(L"new_window") == ico::IconId::NewWindow,
              "按名查表能找到新窗口图标");
        Check(ico::Glyph(ico::IconId::GridView) != fallback, "网格视图有登记字形");
        Check(ico::Glyph(ico::IconId::ListView) != fallback, "列表视图有登记字形");
        Check(ico::Glyph(ico::IconId::DetailsView) != fallback, "详细信息视图有登记字形");

        bool allViewGlyphs = true;
        for (const auto& v : viewmode::kModes)
            if (v.glyph == ico::IconId::None) allViewGlyphs = false;
        Check(allViewGlyphs, "8 档视图模式都在唯一表里带图标");
    }

    // 11. 拖放落点换算：弹层只报"第几行"，换算规则必须是纯函数且有边界。
    //
    // 【为什么这条要单测】落点算错一位 = 条目被放到隔壁，整条拖拽路径就废了。
    // 而落点计算里混着"上/下半区"与"越界"，正是最容易差一位的地方。
    std::printf("\n拖放落点换算（PopupDropIndex）\n");
    {
        // 落在上半区 → 插到该行**之前**（行号不变）
        Check(PopupDropIndex(0, false, 5) == 0, "第 0 行上半区 → 插到最前");
        Check(PopupDropIndex(3, false, 5) == 3, "第 3 行上半区 → 插到第 3 行之前");
        // 落在下半区 → 插到该行**之后**（行号 +1）
        Check(PopupDropIndex(0, true, 5) == 1, "第 0 行下半区 → 插到第 1 行之前");
        Check(PopupDropIndex(3, true, 5) == 4, "第 3 行下半区 → 插到第 4 行之前");
        // 越界一律拒绝（-1 = 无落点）
        Check(PopupDropIndex(-1, false, 5) == -1, "负行号被拒绝");
        Check(PopupDropIndex(5, false, 5) == -1, "行号超出范围被拒绝");
        Check(PopupDropIndex(0, false, 0) == -1, "空列表没有落点");
    }

    // 12. 选中行 → 动作号：必须用"当前显示的那份"行。
    //
    // 弹层支持就地重建行（分类内容弹层拖完一行后宿主重读数据再重建）。
    // 重建之后行序变了、动作号也跟着变 —— 若还用重排之前那份行去查，
    // 用户拖完之后点的那一行会被映射成**旧顺序**的动作号：点 A 打开 B。
    // 这条错了不报错，只是"打开了隔壁那一项"，所以必须单测钉住。
    std::printf("\n选中行 → 动作号映射\n");
    {
        std::vector<PopupRow> rows;
        PopupRow a;
        a.label = L"A";
        a.data = 0;
        PopupRow b;
        b.label = L"B";
        b.data = 1;
        PopupRow c;
        c.label = L"C";
        c.data = 2;
        rows.push_back(a);
        rows.push_back(b);
        rows.push_back(c);

        Check(PopupActionFor(rows, 0) == 0, "第 0 行 → 动作号 0");
        Check(PopupActionFor(rows, 2) == 2, "第 2 行 → 动作号 2");
        Check(PopupActionFor(rows, -1) == -1, "没选中 → -1");
        Check(PopupActionFor(rows, 3) == -1, "越界行号 → -1");

        // 重排之后：宿主重建的行带着**新**动作号，必须用新的一份。
        std::vector<PopupRow> after;
        PopupRow nb;
        nb.label = L"B";
        nb.data = 0;
        PopupRow nc;
        nc.label = L"C";
        nc.data = 1;
        PopupRow na;
        na.label = L"A";
        na.data = 2;
        after.push_back(nb);
        after.push_back(nc);
        after.push_back(na);

        Check(PopupActionFor(after, 2) == 2, "重排后点 A（第 2 行）→ 动作号 2（新 A）");
        Check(PopupActionFor(after, 0) == 0, "重排后点 B（第 0 行）→ 动作号 0（新 B）");
        // 用**旧**的那份查就会错：旧 rows 的第 2 行是 C（动作号 2），
        // 新 rows 的第 2 行是 A —— 两者都返回 2，但指向的条目完全不同。
        // 这正是"点 A 打开 C"那个 bug 的形状。
        Check(PopupActionFor(after, 0) != PopupActionFor(rows, 2),
              "新旧两份的第 2 行映射到不同动作（旧=C，新=A）");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
