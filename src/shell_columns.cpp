#include "shell_columns.h"

#include <algorithm>

#include "shell_ops.h"

namespace shellcolumns {
namespace {

ComPtr<IColumnManager> GetColumnManager(IShellView* view) {
    ComPtr<IFolderView2> fv2 = shellops::FolderView2(view);
    if (!fv2) return ComPtr<IColumnManager>();
    ComPtr<IColumnManager> cm;
    if (FAILED(fv2->QueryInterface(IID_IColumnManager, reinterpret_cast<void**>(&cm))))
        return ComPtr<IColumnManager>();
    return cm;
}

std::vector<PROPERTYKEY> GetKeys(IColumnManager* cm, CM_ENUM_FLAGS which) {
    UINT n = 0;
    if (!cm || FAILED(cm->GetColumnCount(which, &n)) || !n)
        return std::vector<PROPERTYKEY>();
    std::vector<PROPERTYKEY> keys(n);
    if (FAILED(cm->GetColumns(which, keys.data(), n))) return std::vector<PROPERTYKEY>();
    return keys;
}

}  // namespace

std::wstring ColumnIdToString(const PROPERTYKEY& key) {
    wchar_t guid[64] = L"";
    if (!StringFromGUID2(key.fmtid, guid, static_cast<int>(_countof(guid)))) return L"";
    return std::wstring(guid) + L":" + std::to_wstring(key.pid);
}

bool ColumnIdFromString(const std::wstring& id, PROPERTYKEY* key) {
    if (!key || id.size() < 6) return false;
    const size_t colon = id.rfind(L':');
    if (colon == std::wstring::npos || colon < 3 || colon + 1 >= id.size()) return false;
    if (id[0] != L'{' || id[colon - 1] != L'}') return false;

    const std::wstring guid = id.substr(0, colon);
    if (FAILED(IIDFromString(guid.c_str(), &key->fmtid))) return false;

    const std::wstring pidText = id.substr(colon + 1);
    wchar_t* end = nullptr;
    const unsigned long pid = wcstoul(pidText.c_str(), &end, 10);
    if (!end || *end != L'\0' || pid > 0xFFFFFFFFUL) return false;
    key->pid = static_cast<DWORD>(pid);
    return true;
}

bool List(IShellView* view, std::vector<ColumnDesc>* out) {
    if (!out) return false;
    out->clear();
    ComPtr<IColumnManager> cm = GetColumnManager(view);
    if (!cm) return false;

    const std::vector<PROPERTYKEY> keys = GetKeys(cm.get(), CM_ENUM_ALL);
    if (keys.empty()) return false;
    for (const PROPERTYKEY& key : keys) {
        CM_COLUMNINFO ci{};
        ci.cbSize = sizeof(ci);
        ci.dwMask = CM_MASK_NAME | CM_MASK_STATE | CM_MASK_WIDTH;
        if (FAILED(cm->GetColumnInfo(key, &ci))) continue;

        ColumnDesc d;
        d.key = key;
        d.id = ColumnIdToString(key);
        d.name = ci.wszName[0] ? ci.wszName : d.id;
        d.visible = (ci.dwState & CM_STATE_VISIBLE) != 0;
        d.canHide = (ci.dwState & CM_STATE_FIXEDWIDTH) == 0;
        d.width = ci.uWidth;
        out->push_back(std::move(d));
    }
    return !out->empty();
}

bool SetVisible(IShellView* view, const std::vector<std::wstring>& visible) {
    ComPtr<IColumnManager> cm = GetColumnManager(view);
    if (!cm) return false;

    std::vector<ColumnDesc> cols;
    if (!List(view, &cols)) return false;

    // 未知 id 必须硬失败：否则拼错一个 guid 就会把全部列都关掉。
    for (const std::wstring& id : visible) {
        const bool known = std::any_of(cols.begin(), cols.end(),
                                       [&](const ColumnDesc& c) { return c.id == id; });
        if (!known) return false;
    }

    std::vector<PROPERTYKEY> next;
    std::vector<std::wstring> added;
    auto keep = [&](const PROPERTYKEY& key, const std::wstring& id, bool currentlyVisible,
                    bool canHide) {
        const bool wanted = std::find(visible.begin(), visible.end(), id) != visible.end();
        if (!wanted && currentlyVisible && !canHide) return;  // 固定列（名称）不能藏
        if (!wanted) return;
        next.push_back(key);
        added.push_back(id);
    };

    // 先按当前可见顺序保留，再按可用列顺序追加新打开的列 —— 用户熟悉的列顺序不被打乱。
    for (const PROPERTYKEY& key : GetKeys(cm.get(), CM_ENUM_VISIBLE)) {
        const auto it = std::find_if(cols.begin(), cols.end(), [&](const ColumnDesc& c) {
            return IsEqualPropertyKey(c.key, key);
        });
        if (it != cols.end()) keep(it->key, it->id, true, it->canHide);
    }
    for (const ColumnDesc& c : cols)
        if (std::find(added.begin(), added.end(), c.id) == added.end())
            keep(c.key, c.id, false, c.canHide);

    if (next.empty()) return false;
    return SUCCEEDED(cm->SetColumns(next.data(), static_cast<UINT>(next.size())));
}

bool Toggle(IShellView* view, const std::wstring& id, bool* nowVisible) {
    std::vector<ColumnDesc> cols;
    if (!List(view, &cols)) return false;

    const auto it = std::find_if(cols.begin(), cols.end(),
                                 [&](const ColumnDesc& c) { return c.id == id; });
    if (it == cols.end() || !it->canHide) return false;

    std::vector<std::wstring> visible;
    for (const ColumnDesc& c : cols)
        if (c.visible && c.id != id) visible.push_back(c.id);
    if (!it->visible) visible.push_back(id);

    if (!SetVisible(view, visible)) return false;
    if (nowVisible) *nowVisible = !it->visible;
    return true;
}

const std::vector<KnownProperty>& KnownProperties() {
    // 函数内静态：避免跨 TU 的初始化顺序问题；第一次调用才构造 id 串。
    static const std::vector<KnownProperty> table = [] {
        struct Entry {
            PROPERTYKEY key;
            const wchar_t* title;
        };
        const Entry entries[] = {
            {PKEY_ItemNameDisplay, L"名称"},
            {PKEY_DateModified, L"修改日期"},
            {PKEY_DateCreated, L"创建日期"},
            {PKEY_DateAccessed, L"访问日期"},
            {PKEY_ItemTypeText, L"类型"},
            {PKEY_Size, L"大小"},
            {PKEY_Author, L"作者"},
            {PKEY_Title, L"标题"},
            {PKEY_Company, L"公司"},
            {PKEY_Comment, L"注释"},
            {PKEY_Keywords, L"标记"},
            {PKEY_FileDescription, L"文件说明"},
            {PKEY_OriginalFileName, L"原始文件名"},
            {PKEY_ContentType, L"内容类型"},
            {PKEY_FileName, L"文件名"},
        };
        std::vector<KnownProperty> out;
        out.reserve(sizeof(entries) / sizeof(entries[0]));
        for (const auto& e : entries)
            out.push_back({e.key, ColumnIdToString(e.key), e.title});
        return out;
    }();
    return table;
}

}  // namespace shellcolumns
