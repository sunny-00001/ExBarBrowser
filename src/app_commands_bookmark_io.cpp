// 书签文件 IO：bmk.import / bmk.export / bmk.reload。
//
// 【为什么单独成文件】这三条的作用域是**书签文件**，不是书签栏的显示/编辑。
// 人侧走文件对话框与模式弹层（UI 适配器），机器侧给完整路径调确定式后端；
// 两侧共用 bookmark_io.cpp 的同一份读写实现。混在 bookmark 编辑族里会让
// "改一条书签"和"整棵树落盘"两种语义挨在一起，读的人很难看出边界。
#include "app.h"

#include "app_commands.h"
#include "bookmark_io.h"
#include "bookmarks.h"

void App::RegisterBookmarkIoCommands() {
    auto reg = cmd_reg::Register;
    auto regR = cmd_reg::RegisterWithResult;

    regR(CmdId::BmkImport, [this](const Args& args, json::Value* out) -> bool {
        int n = -1;
        if (IsHumanCall(args)) {
            n = BmkImportHtml(hwnd_, theme_);
            if (n == -1) {
                CommandBus::Get().NoteFailure(L"用户取消导入", FailKind::CanceledByUser);
                return false;
            }
        } else {
            const std::wstring path = args.Str(L"path");
            if (path.empty()) {
                CommandBus::Get().NoteFailure(L"缺少必填参数：path");
                return false;
            }
            const int mode = args.Int(L"mode", -1);
            if (mode != 0 && mode != 1) {
                CommandBus::Get().NoteFailure(L"mode 只能是 0（追加）或 1（覆盖）");
                return false;
            }
            n = BmkImportHtmlFromFile(path, mode);
        }
        if (n < 0) {
            if (n == -2) CommandBus::Get().NoteFailure(L"文件无效或没有可导入的书签");
            else if (n == -3) CommandBus::Get().NoteFailure(
                L"书签文件已被其它程序修改，请先重新载入再导入", FailKind::LockConflict);
            else CommandBus::Get().NoteFailure(L"无法打开导入文件");
            return false;
        }
        bookmarkBar_.ReloadExternal();
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"imported", json::Value::MakeInt(n));
            *out = std::move(root);
        }
        return true;
    });

    regR(CmdId::BmkExport, [this](const Args& args, json::Value* out) -> bool {
        int n = -1;
        if (IsHumanCall(args)) {
            n = BmkExportHtml(hwnd_, theme_);
        } else {
            const std::wstring path = args.Str(L"path");
            if (path.empty()) {
                CommandBus::Get().NoteFailure(L"缺少必填参数：path");
                return false;
            }
            n = BmkExportHtmlTo(path);
        }
        if (n < 0) {
            if (n == -1) {
                CommandBus::Get().NoteFailure(L"用户取消导出", FailKind::CanceledByUser);
            } else {
                CommandBus::Get().NoteFailure(L"写入文件失败（路径无效或文件被占用？）");
            }
            return false;
        }
        if (out) {
            json::Value root = json::Value::MakeObject();
            root.Set(L"exported", json::Value::MakeInt(n));
            *out = std::move(root);
        }
        return true;
    });

    reg(CmdId::BmkReload, [this](const Args&) {
        BookmarkStore& store = BookmarkStore::Get();
        bookmarkBar_.ReloadFromDisk();
        if (store.LoadFailed()) {
            CommandBus::Get().NoteFailure(L"书签文件读取失败，已保留内存中的书签");
            return false;
        }
        return true;
    });
}
