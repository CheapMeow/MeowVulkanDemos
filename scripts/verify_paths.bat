@echo off
chcp 65001 > nul
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\verify_report.txt

if not exist "%EXE%" (
    echo 请先执行 scripts\build.bat
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

for %%N in (1000 20000 200000 1000000) do (
    "%EXE%" --instances %%N --far 200 --auto-exit 4 --no-interface --report "%REPORT%"
    "%EXE%" --instances %%N --far 200 --instanced --auto-exit 4 --no-interface --report "%REPORT%"
    "%EXE%" --instances %%N --far 200 --indirect --auto-exit 4 --no-interface --report "%REPORT%"
)

echo 三条路径在各实例数量下的可见实例数量:
type "%REPORT%"
exit /b 0
