# CHANGELOG 0.2.0 详细记录

> 从主 CHANGELOG.md 迁出，保留该版本完整开发过程（验证/踩坑/逐条 why）。

## [0.2.0] - 2026-11-19

**功能迁移**：把 ExBar mod 的七类自定义能力迁到独立进程（M0–M7）。宿主接线（DUI/XAML 子类化、  
低级钩子、跨进程 RPC）不迁移，改为自有窗口 + 组件 + 命令管线。

### 新增

- **命令管线**（`commands.cpp`）：127 条命令目录（与 `data/capabilities.json` 同 id，90 条已实现 / 36 条占位），  
  统一 `Invoke(id, arg, actor)`；`audit.log` 记录 actor（key/seq/mouse/ui/palette/ai）。
- **键盘快捷键**（`keymap.cpp` / `chord.cpp`）：直接读 mod 的 `keymap.json`（**70 条**），  
  支持用户覆盖、冲突与系统保留键标记。
- **序列键**（`sequence.cpp` / `sequence_hint.cpp`）：**38 条**（`g g` / `g t` / `g u` …），  
  前缀状态机 + 提示弹层 + 超时 + `Esc` 取消。
- **鼠标规则**（`mouse_rules.cpp` / `input_router.cpp`）：目标区 × 按钮 × 触发（单击/双击/长按/四向滚轮）  
  × 修饰键；内置"双击书签栏空白 → 切换书签栏"。
- **标签栏**（`tab_strip.cpp` / `tab_set.cpp`）：拖拽排序、锁定、关闭历史、MRU、溢出滚动与 `»` 菜单、  
  右键菜单、克隆、中键关闭。
- **导航栏**：地址自动补全、最近访问、视图排序/分组、隐藏文件三态、导航窗格开关。
- **书签栏**（`bookmark_bar.cpp` / `bookmarks.cpp`）：与 mod 共用 `bookmarks.html`  
  （Netscape + `EXBAR_*`），拖拽添加、右键增删改、显隐。
- **状态栏**（`status_bar.cpp` / `view_stats.cpp`）：9 段（文件夹/文件/已选、修改日期、路径、  
  选中容量、磁盘/容器、隐藏三态、类型），tooltip、段点击、右键显隐。
- **命令面板**（`app_commands.cpp`）与**快捷键管理器**（`hotkey_manager.cpp`）、  
  **文本输入对话框**（`prompt.cpp`，书签重命名/新建分类）。
- **配置与状态**（`config.cpp` / `json.cpp` / `text_file.cpp`）：`%APPDATA%\ExBar\browser.json`，  
  首次运行把 keymap/mouse/bookmarks 复制到用户目录。
- **会话**：保存 / 恢复命名标签组。

### 修复（2026-11-19 代码审查）

- `build.bat` 与 `build.sh` 的文件清单/库列表同步，并补 `-lpropsys`（PKEY 需要）。
- 标签栏作用域的 `Shift+←/→`（`TabMoveLeft/Right`）不再抢文件视图的 Shift 选择：  
  只在"无 Ctrl/Alt 且焦点位于标签栏"时生效；编辑框内的 `Ctrl+C/V/X/A/Z`、方向键、Delete 让给 EDIT。
- `tf::WriteText` 改为写 `.tmp` + `MoveFileEx(REPLACE_EXISTING)` 原子替换，避免写一半截断配置/书签。
- `ShellView::Sink` 补 `virtual ~Sink()`（`Release()` 里 `delete this` 的基类析构要求）。
- 退出顺序修正：`WM_CLOSE` 只保存配置并销毁视图；字体与图标在子窗口销毁后的 `WM_DESTROY` 释放，  
  避免销毁过程中重绘用到已删字体。
- 键位/鼠标用户文件改用 `keymap.json` / `mouse.json`，不再与 mod 的  
  `%APPDATA%\ExBar\keymap.json`（`bindings[]` 格式）互相覆盖。
- 删除未使用的 `app_commands.cpp::ArgInt` 与 `tab_strip.cpp` 的重复 `DrawChevron`。
- 标签栏溢出时不再每次刷新把横向滚动位置重置到最左；切换/新建标签会自动把激活标签滚入可视区。
- 状态栏取数节流：导航变化立即取数；后台窗口每 5 拍、>800 项每 2 拍、>3000 项每 4 拍，  
  避免每 800ms 全量枚举大目录。
- `audit.log` 增加轮转（每 200 条检查一次，超过 2 MB 重命名为 `audit.log.1`，只保留一份）。
- `tabNewPos` 生效：`right`（默认，当前标签右侧）/ `end`（末尾），新建、克隆、右键新建都遵守。
- 修复：地址栏右侧 `⌄` 下拉被长路径撑得比地址框宽 → `ShowPopupList` 新增 `exactWidth`，  
  该菜单严格等于地址框宽度（文字用省略号收）；超出屏幕时保持**左边缘对齐**（缩窄而不是整体左移）。
- 修复：保存的窗口几何可能落在屏幕外（地址框右端跑出屏幕，弹层只能被拉回屏内，看起来不对齐）  
  → 启动时把窗口整体夹回当前显示器工作区。

### 已知问题

- 审计与调试日志都是同步 `CreateFile/WriteFile`（已加 2 MB / 4 MB 轮转，高频操作仍有可测开销）。
- 多个文件超过 300 行（`app.cpp` / `app_commands.cpp` / `tab_strip.cpp` / `bookmark_bar.cpp`…）。
- 代码为平铺结构，与 [docs/ARCHITECTURE-RULES.md](docs/ARCHITECTURE-RULES.md) 的分层规则不一致（待定）；  
  |  |  |  |  
  | --- | --- | --- |  
  `src/contract|input|pipeline` 是并行架构批次的占位，**未接入构建**，与 `src/commands.*` 并存待合并。
- 未实现：搜索（Everything）、工具集、预览/详细信息窗格、复选框、AI 面板、批量选择/复制到/移动到。

---
