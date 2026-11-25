// 序列总览弹窗的取数与派发（App 分片）。
//
// 参考 mod 的 ExBarSeqOverlay：一个弹层承载「模式列表 + 打字过滤 + 回车派发」。
// 本文件只把本项目已有的数据源（命令目录 / 书签 / 标签 / 导航历史 / 最近访问 /
// 搜索历史 / 当前目录子项）铺成弹窗认识的 (标题, 副标题, 命令, 参数) 行，
// 不自己实现任何业务：回车仍走 CommandBus，审计与人机等价口径不变。
#include "app.h"

#include <algorithm>

#include "bookmarks.h"
#include "cmd_naming.h"
#include "commands.h"
#include "config.h"
#include "input_router.h"
#include "nav_path.h"
#include "str.h"
#include "text_file.h"   // tf::ToExtended（长路径前缀）

namespace {

using Row = SequenceOverlay::Row;

// 命令 → 触发（快捷键 · 序列键），对齐原版 SeqBuildTriggerIndex。
//
// 【为什么要走 CommandDef::id 比对】键位表里写的是**旧名**（TabClone），
// 命令目录里 d.name 已是**点分名**（tab.clone）。直接比字符串永远不相等
// （实测：命令面板的「触发」列整列为空）。统一解析成 CommandDef 再比 id，
// 两个写法都能命中。
//
// 【口径】原版是「快捷键 · 序列键」，同一命令多条绑定时按手势字典序排序；
// 序列手势保留原版的空格分隔写法（"g g"，不是压缩后的 "gg"）。
std::wstring TriggerOf(const std::vector<BindingRow>& rows,
                       const std::vector<SequenceBinding>& seqs,
                       const CommandDef* target) {
    if (!target) return std::wstring();
    std::vector<std::wstring> keys, seqTokens;
    for (const auto& r : rows) {
        if (r.isSequence) continue;
        std::wstring name, arg;
        SplitCommandSpec(r.command, &name, &arg);
        const CommandDef* d = FindCommand(name);
        if (!d || d->id != target->id) continue;
        if (r.key.empty() || r.key == L"（未绑定）") continue;
        keys.push_back(r.key);
    }
    for (const auto& sq : seqs) {
        if (sq.tokens.empty()) continue;
        const CommandDef* d = FindCommand(sq.command);
        if (!d || d->id != target->id) continue;
        seqTokens.push_back(sq.seq);   // 原版写法："g g"
    }
    std::sort(keys.begin(), keys.end());
    std::sort(seqTokens.begin(), seqTokens.end());
    std::wstring v;
    for (const auto& t : keys) { if (!v.empty()) v += L" · "; v += t; }
    for (const auto& t : seqTokens) { if (!v.empty()) v += L" · "; v += t; }
    return v;
}

void AddCmd(std::vector<Row>& out, const CommandDef& d, const std::wstring& trigger) {
    Row r;
    r.title = d.title;
    const std::wstring canon = cmdnaming::CanonicalName(d.name);
    r.subtitle = canon;                              // 副标题：点分名（原版放命令 id）
    r.sub = canon + L"  [" + d.group + L"]";         // 旧的两列口径（仍被帮助列表用）
    r.group = cmdnaming::GroupDisplayName(d.group);  // 分组列：命令目录分组
    r.trigger = trigger;                             // 触发列：快捷键 · 序列键
    r.command = d.name;
    r.isCommand = true;                              // 高亮且未绑定时显示「未绑定」
    out.push_back(std::move(r));
}

// 模式行（唤起态的模式列表 / `?` 帮助模式共用同一批数据，只是渲染不同）。
//   · 标题 = "名称 (前缀)"（原版同款）
//   · 副文本 = 说明（唤起态右对齐成「说明」列，帮助模式跟在标题后面）
//   · 回车 = 进入该模式（command = "@mode"，弹层不关）
void AddMode(std::vector<Row>& out, const SequenceOverlay::Mode& m, bool withSubtitle) {
    Row r;
    r.title = std::wstring(m.name) + L" (" + m.prefix + L")";
    r.sub = m.hint;
    if (withSubtitle) r.subtitle = m.hint;
    r.command = L"@mode";
    r.arg = m.name;
    out.push_back(std::move(r));
}

// 命令组匹配过滤：把"bmk"之类的组名也当成可搜索文本。
bool GroupMatch(const CommandDef& d, const std::wstring& needle) {
    std::wstring hay = std::wstring(d.name) + L" " + d.group + L" " +
                       cmdnaming::CanonicalName(d.name) + L" " + d.title;
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = towlower(c);
    for (auto& c : n) c = towlower(c);
    size_t pos = 0;
    for (wchar_t c : n) {
        pos = h.find(c, pos);
        if (pos == std::wstring::npos) return false;
        ++pos;
    }
    return true;
}

}  // namespace

void App::SetupSequenceOverlay() {
    input_.SetOverlayModeOpener([this](const std::wstring& mode) {
        ShowSequenceOverlay(mode);
    });
    input_.InitSequenceOverlay(
        [this](const std::wstring& mode, const std::wstring& filter) {
            return SequenceRows(mode, filter);
        },
        [this](const Row& row) { SequenceExecute(row); });
}

void App::ShowSequenceOverlay(const std::wstring& mode) {
    input_.ShowSequenceOverlay(mode);
}

std::vector<Row> App::SequenceRows(const std::wstring& mode, const std::wstring& filter) {
    std::vector<Row> out;

    // 序列键模式：列出**全部**序列绑定，5 列渲染
    // （序号│序列键│描述│命令│分组），对齐原版 ExBarSeqOverlay。
    //
    // 【为什么不按前缀在这里筛】前缀过滤由弹窗自己做 —— 因为"序列键列宽按
    // **全部**序列里最长 token 算"（原版 SeqMaxKeyChars）需要拿到全量数据，
    // 在这里筛过就没有这个数据源了，列宽会随过滤跳变。
    if (mode == SequenceOverlay::kSeqKeyMode) {
        // 【排序】按序列 token 字典序（原版 QuerySequence 同款：固定排序，
        // 否则候选位置、↑↓ 导航与肌肉记忆都不稳定）。
        // 参考图里的顺序正是这个：g1 g2 … g9 gb gc gd … gm。
        auto seqs = input_.Sequences();
        std::sort(seqs.begin(), seqs.end(),
                  [](const SequenceBinding& a, const SequenceBinding& b) {
                      return a.seq < b.seq;
                  });
        for (const auto& sq : seqs) {
            if (sq.tokens.empty()) continue;
            // token 显示：完整序列压缩形式（"g g" → "gg"，原版 SeqCompact 口径）。
            std::wstring token;
            for (const auto& t : sq.tokens) token += t;
            const CommandDef* d = FindCommand(sq.command);
            Row r;
            r.key = token;
            r.title = d ? d->title : sq.command;
            // 命令 id 列：统一用**本项目的规范名**（点分）。
            // 【为什么不能直接用键位表里的原串】键位表里新旧名混着写
            // （GoTab1 是旧名，go.desktop 是点分名），同一列出现两种写法
            // 就是两套观感。点分名是本项目的命令契约（docs/NAMING.md），
            // 外部客户端、AI、脚本都以它为准。
            r.cmdId = d ? cmdnaming::CanonicalName(d->name) : sq.command;
            r.group = d ? cmdnaming::GroupDisplayName(d->group) : L"";  // 中文分组
            r.command = sq.command;                     // 回车派发
            out.push_back(std::move(r));
        }
        return out;
    }

    // 唤起态（Ctrl+Shift+P）：**模式列表**。
    // 原版 SeqUiUserOpenOnHookThread：唤起时未选模式、图标不亮 → 判据落到
    // 「模式列表」。命令列表是 `!` 前缀进的模式，不在唤起态里。
    if (mode == SequenceOverlay::kUserMode) {
        for (const auto& m : SequenceOverlay::Modes()) AddMode(out, m, false);
        return out;
    }

    // 命令模式：全部已实现命令（几百个操作，一条不漏）。
    //
    // 【排序】原版 SeqFetchTargets 对命令行走 stable_sort：先按**分组 id**、再按标题，
    // "同类相邻、位置稳定"。注意比的是英文分组 id（Tab/Go/Nav/…），不是中文展示名 ——
    // 中文名按码位排会得到"书签·会话·外部·工具…"这种读起来没规律的顺序。
    if (mode == L"命令") {
        const auto& rows = input_.Rows();
        const auto& seqs = input_.Sequences();
        std::vector<const CommandDef*> list;
        for (const auto& d : CommandTable())
            if (d.implemented && GroupMatch(d, filter)) list.push_back(&d);
        std::stable_sort(list.begin(), list.end(),
                         [](const CommandDef* a, const CommandDef* b) {
                             // 分组 id 大小写不敏感（str::ICmp 是本项目唯一的
                             // 不区分大小写比较入口 —— 裸 _wcsicmp 会被
                             // check_components 判违规）。
                             const int c = str::ICmp(a->group, b->group);
                             if (c != 0) return c < 0;
                             // title 是 const wchar_t* —— 必须 wcscmp。
                             // 直接 `a->title < b->title` 比的是**指针**，
                             // 排序结果随机（同组内顺序每次运行都可能变）。
                             return wcscmp(a->title, b->title) < 0;
                         });
        for (const CommandDef* d : list)
            AddCmd(out, *d, TriggerOf(rows, seqs, d));
        return out;
    }

    if (mode == L"书签") {
        for (const auto& leaf : BookmarkStore::Get().Flatten()) {
            Row r;
            r.title = leaf.first;
            r.sub = leaf.second;
            // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
            r.rightText = leaf.second;                 // 右列：完整路径
            r.iconPath = leaf.second;                  // 图标：书签目标的壳图标
            r.command = L"fs.open";
            r.arg = leaf.second;
            out.push_back(std::move(r));
        }
        return out;
    }

    if (mode == L"标签") {
        for (int i = 0; i < tabSet_.Count(); ++i) {
            const auto* t = tabSet_.Find(tabSet_.IdAt(i));
            if (!t) continue;
            Row r;
            r.title = core::DisplayTitle(*t);
            r.sub = t->path;
            // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
            r.rightText = t->path;
            r.iconPath = t->path;                     // 图标：标签所在目录
            r.command = L"tab.activate";
            r.arg = std::to_wstring(i);
            r.data = i;
            out.push_back(std::move(r));
        }
        return out;
    }

    if (mode == L"历史" || mode == L"最近文件") {
        for (const auto& p : Config::Get().s().recentPaths) {
            Row r;
            r.title = nav::Leaf(p);
            r.sub = p;
            // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
            r.rightText = p;
            r.iconPath = p;
            r.command = L"fs.open";
            r.arg = p;
            out.push_back(std::move(r));
        }
        return out;
    }

    if (mode == L"后退栈" || mode == L"前进栈") {
        bool back = (mode == L"后退栈");
        for (const auto& p : HistoryList(back)) {
            Row r;
            r.title = nav::Leaf(p);
            r.sub = p;
            // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
            r.rightText = p;
            r.iconPath = p;
            r.command = L"fs.open";
            r.arg = p;
            out.push_back(std::move(r));
        }
        return out;
    }

    if (mode == L"快速跳转") {
        // 与 mod 同口径：书签 → 历史 → 标签，按目标去重、保留首次。
        std::vector<std::wstring> seen;
        auto push = [&](const std::wstring& title, const std::wstring& target) {
            if (target.empty()) return;
            for (const auto& s : seen)
                if (str::IEquals(s.c_str(), target.c_str())) return;
            seen.push_back(target);
            Row r;
            r.title = title;
            r.sub = target;
            // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
            r.rightText = target;
            r.iconPath = target;
            r.command = L"fs.open";
            r.arg = target;
            out.push_back(std::move(r));
        };
        for (const auto& leaf : BookmarkStore::Get().Flatten()) push(leaf.first, leaf.second);
        for (const auto& p : Config::Get().s().recentPaths) push(nav::Leaf(p), p);
        for (int i = 0; i < tabSet_.Count(); ++i) {
            const auto* t = tabSet_.Find(tabSet_.IdAt(i));
            if (t) push(core::DisplayTitle(*t), t->path);
        }
        return out;
    }

    if (mode == L"搜索" || mode == L"路径") {
        std::wstring q = filter.empty() ? ActivePath() : filter;
        Row r;
        r.title = filter.empty() ? L"（输入搜索词）" : (mode == L"搜索" ? L"搜索：" : L"打开：") + filter;
        r.sub = filter.empty() ? L"输入后回车执行" : filter;
        r.rightText = q;
        r.command = (mode == L"搜索") ? L"search.find" : L"fs.open";
        r.arg = q;
        out.push_back(std::move(r));
        return out;
    }

    if (mode == L"帮助") {
        // 原版 `?` 模式是**目标列表**（不是唤起态那张模式列表）：
        // 序号 │ 图标槽 │ 名称(含前缀) │ 说明(副标题) │ [分组][触发]。
        for (const auto& m : SequenceOverlay::Modes()) AddMode(out, m, true);
        return out;
    }

    // 当前目录 / 筛选：用 shell 现读子目录（只列目录，量可控）。
    if (mode == L"当前目录" || mode == L"筛选") {
        std::wstring dir = ActivePath();
        if (!dir.empty()) {
            std::wstring pattern = dir;
            if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
            pattern += L'*';
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(tf::ToExtended(pattern).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    std::wstring path = dir;
                    if (!path.empty() && path.back() != L'\\') path += L'\\';
                    path += fd.cFileName;
                    Row r;
                    r.title = fd.cFileName;
                    r.sub = path;
                    // 路径行不填副标题：原版目标列表没有"名称后跟父目录"那一列。
                    r.rightText = path;
                    r.iconPath = path;                  // 图标：子目录的壳图标
                    r.command = L"fs.open";
                    r.arg = path;
                    out.push_back(std::move(r));
                } while (FindNextFileW(h, &fd) && out.size() < 300);
                FindClose(h);
            }
        }
        return out;
    }

    return out;
}

void App::SequenceExecute(const Row& row) {
    if (row.command == L"@mode") {
        ShowSequenceOverlay(row.arg);
        return;
    }
    if (!row.command.empty()) {
        // 命令模式（无参数）：走面板统一派发，保留破坏性确认与必填参数追问。
        if (row.arg.empty()) {
            const CommandDef* d = FindCommand(row.command);
            if (d) { InvokeFromPalette(*d); return; }
        }
        CommandBus::Get().InvokeByNameLegacy(row.command, row.arg, Actor::Palette());
        return;
    }
    if (!row.target.empty())
        CommandBus::Get().InvokeByNameLegacy(L"fs.open", row.target, Actor::Palette());
}
