// 书签：树模型 + Netscape 书签 HTML 读写（与 ExBar mod / 浏览器兼容）。
//
// 文件格式（便携模式：<exe>\data\bookmarks.html）：
//   <DT><H3 EXBAR_ID="f-1" EXBAR_FLAGS="0">分类</H3>
//   <DL><p>
//       <DT><A HREF="file:///D:/" EXBAR_ID="b-1" EXBAR_ICON="shell:folder" EXBAR_FLAGS="0">D 盘</A>
//   </DL><p>
// 只依赖 EXBAR_ID（稳定 id），其余属性原样保留以便与 mod 互读。
//
// EXBAR_* 属性名是**跨软件契约**（浏览器/mod/本工程互读），永不改名 —— 见 docs/NAMING.md B2。
#pragma once
#include <windows.h>   // FILETIME（外部变更检测用）；不写它会把 PCH 当依赖

#include <functional>
#include <string>
#include <vector>

struct BookmarkNode {
    std::wstring id;
    std::wstring name;
    std::wstring path;  // 分类为空
    std::wstring icon;  // EXBAR_ICON（可空）
    int flags = 0;      // EXBAR_FLAGS
    bool isFolder = false;
    std::vector<BookmarkNode> children;
};

using BookmarkList = std::vector<BookmarkNode>;

class BookmarkStore {
public:
    static BookmarkStore& Get();

    bool Load(const std::wstring& path);
    // 写盘。overwriteExternal=false（默认）时，若文件已被别的进程改过，
    // **不覆盖**：把内存树另存为 `<file>.conflict.html` 并返回 false，
    // 由 UI 提示用户决定（提示里可以再调 Save(true) 明确覆盖）。
    //
    // 为什么不是"一律拒绝"：拒绝到底等于用户从此再也保存不了书签 ——
    // 一次云同步/一次外部触碰就把功能永久锁死，比丢一次改动更糟。
    // 为什么不是"一律覆盖"：那正是本工程要修的那条数据丢失链。
    // 所以默认保守、明确给出覆盖入口，由人来决定。
    bool Save(bool overwriteExternal = false);

    // 所有 Save 出口（书签栏、拖放、导入、命令面板）的统一回执。
    // 放在 Store 层而不是 BookmarkBar：导入/拖放并不经过 BookmarkBar::SaveStore，
    // 之前它们直接 store.Save() 会绕过失败提示。
    using SaveNotifyFn = std::function<void(bool)>;
    void SetSaveNotifyFn(SaveNotifyFn fn) { saveNotify_ = std::move(fn); }

    const BookmarkList& roots() const { return roots_; }
    BookmarkList& roots() { return roots_; }

    const BookmarkNode* Find(const std::wstring& id) const;
    BookmarkNode* Find(const std::wstring& id);
    bool ContainsPath(const std::wstring& path) const;
    // 按路径找第一条（大小写不敏感，Windows 语义）。
    // 拖放"这个目录已经在书签里了"的处理需要它 —— 只有 ContainsPath 的话，
    // 调用方知道"存在"却拿不到"是哪一条"，于是只能放弃这次拖放（静默无反应）。
    const BookmarkNode* FindByPath(const std::wstring& path) const;

    // 返回新节点 id（失败为空）
    std::wstring AddPath(const std::wstring& name, const std::wstring& path,
                         const std::wstring& parentId = L"");
    std::wstring AddFolder(const std::wstring& name, const std::wstring& parentId = L"");
    // 复制一条书签（分类则连子树深拷贝），插到原条目后面。返回新条目 id（失败为空）。
    // 子树所有 id（含根）重新分配 —— 原样拷贝会让副本与原件撞 id，
    // Find 命中前一条，删除/改名作用到别人身上。
    std::wstring Copy(const std::wstring& id);
    bool Remove(const std::wstring& id);
    bool Rename(const std::wstring& id, const std::wstring& name);
    // 编辑：同时改名与路径（任一为空串表示该项不变）。路径为空且原名非空时视为只改名。
    bool Update(const std::wstring& id, const std::wstring& name, const std::wstring& path);

    // 编辑的"清空也生效"版本：name 非空才改名；path **无条件**写入（含清空）。
    //
    // 为什么需要第二条而不是给 Update 加个 bool：Update 的"空串 = 不改"是给
    // 拖放/导入这类"只知道要改什么"的调用方用的（它们不该把没填的字段清掉）。
    // 编辑框是唯一一个"用户真的把字段清空了"的入口，它的意图必须能表达出来。
    // 把两种语义塞进一个函数 + 一个 bool 参数，会让读代码的人每次都要想
    // "这次传的是 true 还是 false"，而错传的后果是静默丢掉用户输入。
    bool SetNameAndPath(const std::wstring& id, const std::wstring& name,
                        const std::wstring& path);

    // 导入支持：整体替换 / 追加合并（顶层按路径去重，文件夹递归保留）。返回新增条数。
    void ReplaceAll(BookmarkList&& replacement);
    size_t Merge(BookmarkList&& incoming);

    // 移动：插入线语义，把 id 移到**顶层**列表的下标 toIndex 处。返回是否真的移动。
    bool Move(const std::wstring& id, int toIndex);

    // 移动到任意容器（目录 Opus / TC / XYplorer 的"拖进分类"）。
    // targetFolderId 空 = 顶层；index < 0 = 追加到末尾。
    //
    // 拒绝三种情形（都返回 false，不产生撤销快照）：
    //   · 目标不存在 / 不是文件夹；
    //   · 目标就是自己；
    //   · 目标是自己的子孙（把 A 拖进 A\B 会让整棵子树从树上脱落 —— 这是一条
    //     "数据消失"的路径，必须在 store 里拦住，而不是指望 UI 不这么调）。
    bool MoveTo(const std::wstring& id, const std::wstring& targetFolderId, int index = -1);

    // 递归排序（空书签菜单的"按名称排序"）。
    // 递归而不是只排顶层：用户点"按名称排序"看到的是"分类里的顺序纹丝不动"，
    // 那不是排序，是排了一半。文件夹始终排在书签之前（与资源管理器一致）。
    // 会入撤销栈 —— 排序可能是误触，且它改变了用户手工调整过的顺序。
    // recursive=false 只排顶层（给"我只想理一下栏上的顺序"这种明确意图用）。
    void SortRecursive(bool foldersFirst, bool ascending, bool recursive = true);

    // 统计重复路径（同一目录被加了两次；导入合并后可能出现）。
    // 返回去重后的删除条数，并保留首次出现的那一条。
    size_t RemoveDuplicatePaths();

    // 撤销：删除/移动前的快照栈（上限 20 步）。
    bool CanUndo() const { return !undo_.empty(); }
    // 顶快照是否来自机器（AI/脚本）。AI 的撤销只到自己的上一次修改为止，
    // 不穿透界面操作 —— 否则 AI 侧一次批量 undo 能把用户几天前的手工整理
    // 全部滚掉（2026-11-21 结构化审计实测：40 次 undo 穿透回滚了全部历史）。
    bool TopUndoFromMachine() const;
    bool Undo();

    // 外部变更检测：书签文件被其它进程（mod/编辑器）改过后返回 true。
    bool FileChanged() const;

    // 扁平化（只含有 path 的叶子），供序列键/命令面板使用
    std::vector<std::pair<std::wstring, std::wstring>> Flatten() const;

    // 叶子条目总数（Flatten 的长度，不分配）。
    //
    // 为什么单独给一个计数：状态快照每次 AI 调用都要取一次，
    // 用 Flatten().size() 会为了一个数字分配一整个 vector。
    // 口径与 Flatten 一致（只数有 path 的叶子，不数分类）。
    size_t CountLeaves() const;

    // 分类（文件夹）总数（不分配）。
    //
    // 与 CountLeaves 分开的原因：新建分类只加分类、不加叶子 ——
    // 两者的变化方向不同，合成一个数字就写不出"新建分类成功了吗"。
    size_t CountFolders() const;

    // 节点总数（叶子 + 分类，不分配）。
    //
    // 为什么还要第三个口径：删掉一个**空分类**时叶子数不变、
    // 删掉一个叶子时分类数不变 —— 任何一个单独的口径都写不出
    // "删除成功了吗"。节点总数是唯一在两种删除下都变化的量。
    size_t CountNodes() const;

    const std::wstring& file() const { return file_; }
    bool dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    // 上次 Load 是否**因为文件存在但读不懂**而失败（区别于"文件不存在"）。
    //
    // 这个标志存在的唯一目的：阻止 Save() 把一份读不懂的文件覆盖成"空书签"。
    // 用户手工编辑打错一个标签、编辑器保存成 GBK、另一进程正在写 —— 只要
    // Load 失败而文件确实存在，写盘就等于销毁用户全部书签。
    bool LoadFailed() const { return loadFailed_; }

    // 上次 Save 是否因为"文件被外部改过"而拒绝写盘，并另存了 .conflict.html。
    // 一次性：读一次就清。UI 据此提示用户"你的书签没保存，另存在哪里"。
    bool TakeConflictFlag() {
        const bool v = conflictSaved_;
        conflictSaved_ = false;
        return v;
    }

    // 序列化成完整的 Netscape 文档（含表头）。Save 与"冲突另存"共用同一份，
    // 两处各拼一次表头迟早会分叉（一处漏 META charset，浏览器打开就乱码）。
    std::wstring SerializeDocument() const;

private:
    BookmarkStore() = default;
    std::wstring NewId(bool folder);
    BookmarkList* ParentList(const std::wstring& parentId);
    void Touch() { dirty_ = true; }
    void PushUndo();
    FILETIME LastWriteOf(const std::wstring& path) const;
    unsigned long long SizeOf(const std::wstring& path) const;

    SaveNotifyFn saveNotify_;

    BookmarkList roots_;
    std::wstring file_;
    bool dirty_ = false;
    // 文件存在但解析失败 → 禁止写盘（否则会用空树覆盖用户的全部书签）。
    bool loadFailed_ = false;
    // 上次 Save 因外部变更被拒，并把内存树另存成了 .conflict.html。
    bool conflictSaved_ = false;
    int seq_ = 1;
    struct UndoEntry {
        BookmarkList roots;
        bool byMachine = false;   // 快照产生时的 actor（CommandBus::fromMachine）
    };
    std::vector<UndoEntry> undo_; // 删除/重排前快照（≤20，带 actor 标签）
    FILETIME fileStamp_{};                // Load/Save 时的文件时间，用于外部变更检测
    // 与 fileStamp_ 配套的**文件大小**。单看 mtime 会漏判：FAT/网络共享时间戳
    // 粒度可达 2 秒、CopyFile 保留时间戳、SetFileTime 可回拨。
    unsigned long long fileStampSize_ = 0;
};

// HTML 与路径互转（供拖放/复制路径复用）
std::wstring BookmarkPathToUrl(const std::wstring& path);
std::wstring BookmarkUrlToPath(const std::wstring& url);

// shell 命名空间路径的纯格式校验（::{8-4-4-4-12}）。实现见 bookmarks.cpp。
// bmk.add.path / bmk.edit 的入口必须过它：缺 } 的 CLSID 曾经照单落盘，
// 之后"打不开 + 通用图标"两头排查（2026-11-21 真实踩坑两次）。
bool IsValidShellNamespacePath(const std::wstring& path);
