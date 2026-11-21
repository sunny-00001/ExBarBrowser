---
status: reference
baseline: 2026-11-19 20:40
---

# 参考调研：Directory Opus 是怎么组织的

> **前提**：Opus 闭源，内部模块划分不可知。本文只依据**公开契约**（官方手册的脚本对象模型、
> 内部命令参考、插件 API、DOpusRT 参考）反推它的架构骨架，并标注哪些是事实、哪些是推断。
> 目的不是"抄 Opus"，而是回答"我们要不要按它的契约组织代码"。

---

## 1. 事实：Opus 对外暴露的五件事

### 1.1 对象模型：DOpus → Lister → Tab → Item

| 对象 | 职责（官方文档原文要点） | 关系 |
|---|---|---|
| `DOpus` | 全局入口：`listers`、`viewers`、`tabgroups`、`vars`、`FSUtil`、`Create()` | 单例 |
| `Lister` | 一个窗口：`activetab`、`tabs`、`dual`、`layout`、`style`、`toolbars`、窗口坐标 | 1..n |
| `Tab` | 一个文件夹上下文：`path`、`backlist`/`forwardlist`、`format`、`stats`/`selstats`、`lock`、`label`、`linktab` | 属于 Lister |
| `Item` / `Items` | 文件/文件夹与其集合（`all`/`dirs`/`files`/`selected`/`hidden`） | 属于 Tab |
| `Command` / `Func` | 命令执行上下文：`RunCommand`、`sourcetab`/`desttab`、`args`、`results` | 独立对象 |

关键点：**Tab 是有状态的**（路径、双向历史、格式、锁定、颜色、标签），**Lister 只负责窗口与集合**，
**View（视图模式）是 Tab 的 `format` 的一部分**。这与我们现在的 `App::Tab { hist, idx, display, items }`
形状一致——我们缺的是把"窗口"与"标签"分开命名，以及把 `format` 独立出来。

### 1.2 动作入口：内部命令是唯一通道

- 工具栏按钮、菜单项、热键、脚本、外部调用，最终都落到**同一个内部命令表**（`Go`、`Copy`、`Rename`、
  `Select`、`Set`、`Show`…），命令带参数与 `@` 修饰符（`@confirm`、`@sync`、`@keydown`、`@ifset`）。
- 外部调用走 `dopusrt.exe /cmd[:active|viewer] <Command> <args>`；`/cmd` 把命令送进主进程，
  没有窗口时会新开一个。
- 官方明确：`dopusrt.exe` 是"小助手程序"，同一份还会作为崩溃处理器（保存 dump）。

**这是 Opus 最值得抄的一条**：命令不是 UI 的附属，而是**产品 API**。命令面板、快捷键、AI 调用、
外部脚本因此天然统一。

### 1.3 扩展点：按类型分插件，而不是一个万能插件

| 扩展类型 | 形态 | 能力 |
|---|---|---|
| Viewer 插件 | DLL | 预览/缩略图/转换，按扩展名优先序匹配，有 catch-all 类型 |
| VFS 插件 | DLL | 把压缩包/FTP/网盘当目录（读写、batch mode） |
| 元数据 | 内置 + 插件 | 读/写文件属性 |
| 脚本 add-in | `.js` / `.vbs`（可打包 `.osp`） | 监听事件（打开 Lister、切换 Tab、切换文件夹前后、视图模式变化）、注册自定义命令与列、读写自己的配置 |

要点：**每种扩展只拿到一个窄接口**，不共享内部对象；插件有"优先级/启用开关/配置页"三个管理面。

### 1.4 配置：集中 + 声明式

- 配置集中在一个数据目录（`/dopusdata`），支持导出/还原（`.ocb`）。
- 插件通过 XML 配置 API 读写自己的配置。
- 工具栏/菜单是**声明式定义**（不是写死在 C++ 里），所以用户能改、能导入导出。

### 1.5 进程模型：主进程 + 小助手

`dopus.exe`（主，含 UI 与命令引擎）+ `dopusrt.exe`（外部命令入口 + 崩溃处理器）。
好处：外部程序不需要知道主进程的窗口消息/内部协议，只依赖稳定命令行契约。

---

## 2. 推断（标注清楚，不当事实用）

- Opus 内部大概率是"命令引擎 + 对象模型 + 视图抽象"三块，插件围绕视图/VFS 两个抽象扩展；
  具体模块边界、线程模型、渲染方式都无公开资料。
- 它没有走"每个功能一个 DLL"的路线（插件只覆盖 Viewer/VFS/元数据），业务功能仍在内核里，
  说明**功能纵切是代码组织手段，不是运行时边界**——这点对我们很重要：我们的 `features/` 应该是
  编译期目录，不是 DLL。

---

## 3. 抄什么、不抄什么（针对 3,000 行规模的取舍）

| Opus 的做法 | 我们抄吗 | 理由 |
|---|---|---|
| Lister / Tab / View 三层对象模型 | **抄** | 我们已有 `App → Tab → ShellView`，只差命名与职责边界；Tab 持有历史/格式，Lister 持有集合 |
| 命令是唯一动作入口 | **抄** | 现在 `ACCEL` 表 + `WM_COMMAND` 的 switch 就是雏形；提前到 B2，命令面板/AI/外部调用才有落点 |
| 外部调用走独立 helper + 稳定命令行 | **抄一半** | 我们已有路径转发 `WM_COPYDATA`；P3 加 `/cmd` 解析即可，不需要新进程（`ExBarHelper` 是 AI/工具侧，别混） |
| 按类型分插件（Viewer/VFS/元数据） | **不抄** | 我们的"视图"就是 shell 自己；没有 VFS 需求；引 DLL ABI 会带来版本/崩溃面 |
| 脚本 add-in（JS/VBS + 事件） | **暂不抄** | 引入脚本引擎是另一个工程；P3 的命令注册表已能覆盖"自定义命令"的需求 |
| 声明式工具栏/菜单（XML） | **部分抄** | 我们不打算让用户改 chrome；但**命令表应该是数据**（`contract/commands.h` 或 JSON），不是散落的 switch |
| 双栏 / 标签组 / 布局样式 | **不抄** | 单栏单窗口是当前产品定位；会话（P2）只做"标签组"，不做双栏 |
| 集中配置 + 导出还原 | **抄** | P2 的 `%APPDATA%\ExBar` 与 mod 共用，已经是这个方向 |

---

## 4. 映射：Opus 概念 → 我们的现状 → 目标

| Opus | 我们现在 | 目标（规则文档里的名字） |
|---|---|---|
| `DOpus`（全局） | `App::Get()` 单例 | `app::Runtime`（进程级装配） |
| `Lister` | `App`（窗口 + 标签 + 布局 + 消息混在一起） | `app::Lister`（窗口、布局、消息、chrome 装配） |
| `Tab` | `App::Tab { view, hist, idx, items, display }` | `core::Tab`（状态）+ `features::tabs`（生命周期） |
| `Tab.format`（视图模式） | `ShellView::SetViewMode` 直调 | `core::TabFormat`（P2，视图模式/排序/过滤） |
| `View` | `ShellView`（唯一实现） | `platform::shell::ShellView`，将来可加 `PreviewView` |
| `Command` | `ACCEL` 表 + `WM_COMMAND` switch | `contract::CommandSpec` + `app::CommandRegistry` |
| `dopusrt /cmd` | `WM_COPYDATA`（只传路径） | 同一通道扩展为 `/cmd <Command> <args>`（P3） |
| 插件 / 脚本 | 无 | `features/<name>`（编译期）+ `contract::Ports`（窄接口） |
| 配置目录 | 无（P2 计划） | `%APPDATA%\ExBar`，与 mod 同 Schema |

---

## 5. 结论（一句话）

**按 Opus 的"契约"组织，不按它的"实现"组织**：对象模型三层、命令唯一入口、扩展点窄接口；
插件 ABI、脚本引擎、双栏、XML 工具栏都不引入。这样既拿到它的扩展性，又保住我们单 exe、零依赖的形态。

---

## 6. 来源

- Lister 对象：https://docs.dopus.com/doku.php?id=reference:scripting_reference:scripting_objects:lister
- Tab 对象：https://docs.dopus.com/doku.php?id=reference:scripting_reference:scripting_objects:tab
- DOpus 对象：https://docs.dopus.com/doku.php?id=reference:scripting_reference:scripting_objects:dopus
- 脚本对象总览（Opus 12 手册）：https://www.gpsoft.com.au/help/opus12/Documents/Scripting/scriptingobjects.htm
- Script Add-ins 与事件：https://www.gpsoft.com.au/help/opus11/Documents/Opus11/New_Scripting_Interface.htm
- DOpusRT（`/cmd`、崩溃处理）：https://docs.dopus.com/doku.php?id=reference:dopusrt_reference
- 插件类型与 Viewer 插件说明：http://www.pretentiousname.com/opus_plugin_list/ ；
  https://chaoses-ib.gitbook.io/directory-opus/guan-fang-shou-ce/readme.zh-5/readme.zh/readme.zh-18/viewer_plugins.zh
- 插件 XML 配置 API 与兼容性：https://nudel.dopus.com/opus9/page5.html
- 同步/异步命令语义：https://www.gpsoft.com.au/help/opus11/documents/Synchronous_and_Asynchronous_functions.htm
