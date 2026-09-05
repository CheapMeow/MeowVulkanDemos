@echo off
chcp 65001 > nul
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate

if not exist "%EXE%" (
    echo Please run scripts\build.bat first
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set INSTANCES=%1
if "%INSTANCES%"=="" set INSTANCES=200000
set FAR=%2
if "%FAR%"=="" set FAR=160
set SECONDS=%3
if "%SECONDS%"=="" set SECONDS=8

echo === Traditional drawIndexed path ===
"%EXE%" --instances %INSTANCES% --far %FAR% --auto-exit %SECONDS% --no-interface --capture "%OUT_DIR%\traditional.png" > "%OUT_DIR%\traditional.log" 2>&1
type "%OUT_DIR%\traditional.log"

echo === Indirect + compute shader culling path ===
"%EXE%" --instances %INSTANCES% --far %FAR% --indirect --auto-exit %SECONDS% --no-interface --capture "%OUT_DIR%\indirect.png" > "%OUT_DIR%\indirect.log" 2>&1
type "%OUT_DIR%\indirect.log"

echo === Checksums of the frames captured by both paths ===
powershell -NoProfile -Command "Get-FileHash '%OUT_DIR%\traditional.png','%OUT_DIR%\indirect.png' -Algorithm SHA256 | Format-Table -AutoSize Hash, Path"

exit /b 0
