# 第三方声明

本项目自身代码使用 [MIT License](LICENSE)。以下第三方组件随发布包分发或被运行时依赖，版权与许可归各自权利人所有。

## 随包分发

### Microsoft WebView2 Loader

| 项 | 值 |
|---|---|
| 文件 | `WebView2Loader.dll` |
| 产品名 | Microsoft Edge Embedded Browser WebView Loader |
| 版本 | 1.0.2592.51 |
| 版权 | Microsoft Corporation |
| 用途 | 加载系统已安装的 WebView2 Runtime；仅 AI 面板需要 |
| 许可 | Microsoft WebView2 SDK 许可条款 |

许可与再分发说明以 Microsoft 官方文档为准：

- https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution
- https://www.nuget.org/packages/Microsoft.Web.WebView2

本项目未修改该 DLL。发布包中的 `START-HERE.txt` 也会提示其版权归属。

## 运行时依赖（不随包分发）

### WebView2 Runtime

Windows 10 1809+ / Windows 11 可能已预装；未预装时由用户自行安装 Evergreen Runtime。主浏览器不依赖它，仅 AI 面板依赖。

### Node.js

AI 面板的本地服务使用 Node.js ≥18，需用户自行安装或设置 `EXBAR_NODE`。发布包不含 Node.js 二进制，也不含任何 npm 第三方依赖。

## 未使用第三方库

主程序不引入第三方 C/C++ 库、不走 vcpkg/Conan、不打包 npm 依赖。图标由仓库内脚本生成，字体使用系统字体。
