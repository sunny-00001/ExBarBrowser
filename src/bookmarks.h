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
    bool Save();

    const BookmarkList& roots() const { return roots_; }
    BookmarkList& roots() { return roots_; }

    const BookmarkNode* Find(const std::wstring& id) const;
    BookmarkNode* Find(const std::wstring& id);
    bool ContainsPath(const std::wstring& path) const;

    // 返回新节点 id（失败为空）
    std::wstring AddPath(const std::wstring& name, const std::wstring& path,
                         const std::wstring& parentId = L"");
    std::wstring AddFolder(const std::wstring& name, const std::wstring& parentId = L"");
    bool Remove(const std::wstring& id);
    bool Rename(const std::wstring& id, const std::wstring& name);
    // 编辑：同时改名与路径（任一为空串表示该项不变）。路径为空且原名非空时视为只改名。
    bool Update(const std::wstring& id, const std::wstring& name, const std::wstring& path);

    // 导入支持：整体替换 / 追加合并（顶层按路径去重，文件夹递归保留）。返回新增条数。
    void ReplaceAll(BookmarkList&& replacement);
    size_t Merge(BookmarkList&& incoming);

    // 顶层重排：把 id 移到原列表的下标 toIndex 处（插入线语义）。返回是否真的移动。
    bool Move(const std::wstring& id, int toIndex);

    // 撤销：删除/移动前的快照栈（上限 20 步）。
    bool CanUndo() const { return !undo_.empty(); }
    bool Undo();

    // 外部变更检测：书签文件被其它进程（mod/编辑器）改过后返回 true。
    bool FileChanged() const;

    // 扁平化（只含有 path 的叶子），供序列键/命令面板使用
    std::vector<std::pair<std::wstring, std::wstring>> Flatten() const;

    const std::wstring& file() const { return file_; }
    bool dirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    // 上次 Load 是否**因为文件存在但读不懂**而失败（区别于"文件不存在"）。
    //
    // 这个标志存在的唯一目的：阻止 Save() 把一份读不懂的文件覆盖成"空书签"。
    // 用户手工编辑打错一个标签、编辑器保存成 GBK、另一进程正在写 —— 只要
    // Load 失败而文件确实存在，写盘就等于销毁用户全部书签。
    bool LoadFailed() const { return loadFailed_; }

private:
    BookmarkStore() = default;
    std::wstring NewId(bool folder);
    BookmarkList* ParentList(const std::wstring& parentId);
    void Touch() { dirty_ = true; }
    void PushUndo();
    FILETIME LastWriteOf(const std::wstring& path) const;

    BookmarkList roots_;
    std::wstring file_;
    bool dirty_ = false;
    // 文件存在但解析失败 → 禁止写盘（否则会用空树覆盖用户的全部书签）。
    bool loadFailed_ = false;
    int seq_ = 1;
    std::vector<BookmarkList> undo_;      // 删除/重排前快照（≤20）
    FILETIME fileStamp_{};                // Load/Save 时的文件时间，用于外部变更检测
};

// HTML 与路径互转（供拖放/复制路径复用）
std::wstring BookmarkPathToUrl(const std::wstring& path);
std::wstring BookmarkUrlToPath(const std::wstring& url);
