// 导航模型：路径 → 面包屑分段 / 直接子文件夹枚举 / 地址文本
//
// 路径语义（父级/叶子/命名空间/盘根）**不在这里实现** —— 唯一实现在 nav_path.h。
// 本文件只保留必须碰 Shell 的部分：面包屑分段、子文件夹枚举。
// 以前这里有一套基于 PathRemoveFileSpecW + MAX_PATH 的 ParentOf，与 common.h
// 那套规则不一致，且超长路径直接返回空导致地址栏补全静默失效。
#include "nav_model.h"

std::wstring NavModel::LeafOfPath(const std::wstring& path) { return nav::Leaf(path); }

std::wstring NavModel::ParentOf(const std::wstring& path) { return nav::ParentOf(path); }

bool NavModel::IsRoot() const { return !nav::CanNavigateUp(path_); }

std::wstring NavModel::AddressText() const {
    if (nav::IsNamespace(path_) && !display_.empty()) return display_;
    return path_;
}

void NavModel::Clear() {
    path_.clear();
    display_.clear();
    ghostPath_.clear();
    parts_.clear();
}

void NavModel::SetPath(const std::wstring& path, const std::wstring& display) {
    if (path == path_ && display == display_) return;

    // 向上导航时保留"被放弃的更深分支"：新路径正好是旧路径的父级
    if (!path_.empty() && !path.empty() && ParentOf(path_) == path) {
        ghostPath_ = path_;
    } else if (path != path_) {
        ghostPath_.clear();
    }

    path_ = path;
    display_ = display;
    Rebuild();
}

void NavModel::Rebuild() {
    parts_.clear();
    if (path_.empty()) return;

    // 分段规则唯一实现在 nav::Prefixes（纯字符串、可单测）。
    // 以前这里只认 "X:\" 一种形状：UNC 路径与命名空间子项都塌成单段，
    // 面包屑只剩一格 —— 而同一路径的"上一级"按钮是好的，于是"能上不能点"。
    const std::vector<std::wstring> paths = nav::Prefixes(path_);
    for (size_t k = 0; k < paths.size(); ++k) {
        // 命名空间（此电脑/回收站/shell:…）只有一段，且没有文件系统路径语义：
        // 显示文本必须用 shell 给的友好名（与 AddressText 同一口径），
        // 不能拿裸 "::{GUID}" 当路径切、更不能被 CrumbLabel 误判成盘根截成 "::"。
        // display_ 为空（Shell 回调未到）时退回 CrumbLabel，至少不会是空标签。
        const bool nsRoot = (paths.size() == 1 && nav::IsNamespace(path_));
        const std::wstring label =
            (nsRoot && !display_.empty()) ? display_ : nav::CrumbLabel(paths[k], k);
        parts_.push_back({label, paths[k], false});
    }

    if (!ghostPath_.empty()) parts_.push_back({LeafOf(ghostPath_), ghostPath_, true});
}

bool NavModel::ChildFolders(const std::wstring& folder,
                            std::vector<std::pair<std::wstring, std::wstring>>& out) {
    out.clear();
    if (folder.empty()) return false;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;

    // 注意：SHBindToParent 拿到的是"父文件夹 + 本项子 PIDL"；要枚举本文件夹的
    // 子项，必须再 BindToObject 一次拿到它自己的 IShellFolder（否则列的是同级）。
    ComPtr<IShellFolder> parent;
    ComPtr<IShellFolder> sf;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child)) &&
        parent &&
        SUCCEEDED(parent->BindToObject(child, nullptr, IID_IShellFolder,
                                      reinterpret_cast<void**>(&sf))) &&
        sf) {
        ComPtr<IEnumIDList> e;
        if (SUCCEEDED(sf->EnumObjects(nullptr, SHCONTF_FOLDERS, &e)) && e) {
            LPITEMIDLIST id = nullptr;
            while (e->Next(1, &id, nullptr) == S_OK) {
                SFGAOF attr = SFGAO_HIDDEN;
                PCUITEMID_CHILD one = id;
                sf->GetAttributesOf(1, &one, &attr);
                if (attr & SFGAO_HIDDEN) {
                    CoTaskMemFree(id);
                    continue;
                }

                std::wstring name;
                STRRET sr{};
                if (SUCCEEDED(sf->GetDisplayNameOf(id, SHGDN_NORMAL, &sr))) {
                    wchar_t buf[512] = L"";
                    if (SUCCEEDED(StrRetToBufW(&sr, id, buf, 512))) name = buf;
                }

                PIDLIST_ABSOLUTE full = ILCombine(pidl, id);
                std::wstring p = full ? PidlName(full) : std::wstring();
                CoTaskMemFree(id);
                if (full) CoTaskMemFree(full);
                if (!name.empty() && !p.empty()) out.emplace_back(name, p);
            }
        }
    }
    CoTaskMemFree(pidl);

    std::sort(out.begin(), out.end(), [](const std::pair<std::wstring, std::wstring>& a,
                                         const std::pair<std::wstring, std::wstring>& b) {
        return StrCmpLogicalW(a.first.c_str(), b.first.c_str()) < 0;
    });
    return !out.empty();
}
