@echo off
setlocal
REM ExBarBrowser 主编译入口（cmd）
REM 底层统一走 build_fast.py：PCH + 分 TU 增量 + 并行编译。
REM 参数透传，例如：
REM   build.bat --clean          全量重建
REM   build.bat -j 16            16 路并行
REM   build.bat --out x.exe      输出到指定路径（不打断运行实例）
cd /d "%~dp0"

where python >nul 2>nul
if not errorlevel 1 (
  python build_fast.py %*
  goto :check
)
where py >nul 2>nul
if not errorlevel 1 (
  py -3 build_fast.py %*
  goto :check
)
echo BUILD FAILED: 未找到 python（build_fast.py 需要 python3）
exit /b 1

:check
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
endlocal
