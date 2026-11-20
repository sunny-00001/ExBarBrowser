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

    if (nav::IsNamespace(path_)) {
        parts_.push_back({display_.empty() ? path_ : display_, path_, false});
    } else {
        // "C:\A\B" -> ["C:\", "C:\A", "C:\A\B"]
        std::vector<std::wstring> paths;
        if (path_.size() >= 3 && path_[1] == L':' && path_[2] == L'\\') {
            paths.push_back(path_.substr(0, 3));
            size_t i = 3;
            while (i < path_.size()) {
                size_t j = path_.find(L'\\', i);
                size_t end = (j == std::wstring::npos) ? path_.size() : j;
                // 必须是**完整前缀**（C://Windows），不是裸段名（Windows）——
                // 否则分段点击/同级枚举会拿到相对路径，解析失败。
                paths.push_back(path_.substr(0, end));
                if (j == std::wstring::npos) break;
                i = j + 1;
            }
        } else {
            paths.push_back(path_);
        }

        for (size_t k = 0; k < paths.size(); ++k) {
            std::wstring label;
            if (k == 0 && paths[0].size() >= 2 && paths[0][1] == L':')
                label = paths[0].substr(0, 2);  // "C:"
            else
                label = LeafOfPath(paths[k]);
            parts_.push_back({label, paths[k], false});
        }
    }

    if (!ghostPath_.empty()) parts_.push_back({LeafOfPath(ghostPath_), ghostPath_, true});
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
