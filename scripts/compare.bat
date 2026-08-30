@echo off
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate

if not exist "%EXE%" (
    echo 请先执行 scripts\build.bat
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set INSTANCES=%1
if "%INSTANCES%"=="" set INSTANCES=200000
set FAR=%2
if "%FAR%"=="" set FAR=160
set SECONDS=%3
if "%SECONDS%"=="" set SECONDS=8

echo === 传统 drawIndexed 路径 ===
"%EXE%" --instances %INSTANCES% --far %FAR% --auto-exit %SECONDS% --capture "%OUT_DIR%\traditional.png" > "%OUT_DIR%\traditional.log" 2>&1
type "%OUT_DIR%\traditional.log"

echo === indirect + 计算着色器剔除路径 ===
"%EXE%" --instances %INSTANCES% --far %FAR% --indirect --auto-exit %SECONDS% --capture "%OUT_DIR%\indirect.png" > "%OUT_DIR%\indirect.log" 2>&1
type "%OUT_DIR%\indirect.log"

echo === 两条路径输出画面的校验值 ===
powershell -NoProfile -Command "Get-FileHash '%OUT_DIR%\traditional.png','%OUT_DIR%\indirect.png' -Algorithm SHA256 | Format-Table -AutoSize Hash, Path"

exit /b 0
