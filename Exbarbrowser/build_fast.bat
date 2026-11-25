@echo off
REM ExBarBrowser 增量构建入口（cmd 版；底层调用 build_fast.py）
REM 用法：build_fast.bat            增量构建
REM       build_fast.bat --clean   全量重建
REM       build_fast.bat -j 16     指定并行数
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [错误] 未找到 python，请先安装或改用 bash build.sh
  exit /b 1
)

python build_fast.py %*
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
endlocal
