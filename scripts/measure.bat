@echo off
chcp 65001 > nul
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\measure_report.txt

if not exist "%EXE%" (
    echo 请先执行 scripts\build.bat
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

call :measure 200000 160
call :measure 200000 420
call :measure 1000000 160

echo 测量结果:
type "%REPORT%"
exit /b 0

:measure
"%EXE%" --instances %1 --far %2 --auto-exit 8 --no-interface --report "%REPORT%"
"%EXE%" --instances %1 --far %2 --instanced --auto-exit 8 --no-interface --report "%REPORT%"
"%EXE%" --instances %1 --far %2 --indirect --auto-exit 8 --no-interface --report "%REPORT%"
exit /b 0
