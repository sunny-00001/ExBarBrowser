# 安全策略

## 支持版本

| 版本 | 状态 |
|---|---|
| 最新 GitHub Release | 支持 |
| `v0.3.1` | 支持 |
| `v0.3.0` | 已由 `v0.3.1` 取代，不再支持 |
| 更早版本 | 不支持 |

请始终从 GitHub Releases 下载发布包，并核对 `.sha256` 文件。

## 报告安全漏洞

**不要开公开 issue。** 请通过 GitHub 私密安全公告提交：

https://github.com/sunny-00001/ExBarBrowser/security/advisories/new

建议包含：

- 受影响版本 / commit
- 操作系统版本、WebView2 Runtime 版本、Node.js 版本（如涉及 AI 面板）
- 复现步骤与最小证据
- 影响范围（本地提权、信息泄露、文件损坏、远程代码执行等）

处理预期：3 个工作日内确认收到，7 个工作日内给出初步判断与修复计划。

## 敏感数据处理

- `aichat/service/model.json` 中的 `apiKey` 是用户私密数据，仓库和发布包都不携带真实 key；发布包只含 `*.example.json` 模板。
- 不要把真实 `model.json`、`data/settings.json`、书签或日志贴到 issue / PR 中。
- 如不慎泄露 key，请立即在服务商处吊销并轮换；本项目无法撤回已发布的第三方密钥。

## 安全设计边界

- 不注入 explorer，不安装全局钩子（唯一例外是中文输入法场景下的 `WH_KEYBOARD_LL` 低级键盘钩子）。
- 禁止模拟鼠标/键盘、抢焦点（规则 R21）；自动化只走命令总线或只读观察。
- 发布包只携带必要的 `WebView2Loader.dll` 与 `aichat/`，第三方组件见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
