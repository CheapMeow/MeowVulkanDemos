@echo off
chcp 65001 > nul
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

echo === 逐实例 drawIndexed 路径 ===
"%EXE%" --instances %INSTANCES% --far %FAR% --auto-exit %SECONDS% --no-interface --capture "%OUT_DIR%\traditional.png" > "%OUT_DIR%\traditional.log" 2>&1
powershell -NoProfile -Command "Get-Content '%OUT_DIR%\traditional.log' -Encoding UTF8 | Where-Object { $_ -match 'GPU ' } | Select-Object -Last 1"

echo === 实例化 drawIndexed 路径 ===
"%EXE%" --instances %INSTANCES% --far %FAR% --instanced --auto-exit %SECONDS% --no-interface --capture "%OUT_DIR%\instanced.png" > "%OUT_DIR%\instanced.log" 2>&1
powershell -NoProfile -Command "Get-Content '%OUT_DIR%\instanced.log' -Encoding UTF8 | Where-Object { $_ -match 'GPU ' } | Select-Object -Last 1"

echo === indirect + 计算着色器剔除路径 ===
"%EXE%" --instances %INSTANCES% --far %FAR% --indirect --auto-exit %SECONDS% --no-interface --capture "%OUT_DIR%\indirect.png" > "%OUT_DIR%\indirect.log" 2>&1
powershell -NoProfile -Command "Get-Content '%OUT_DIR%\indirect.log' -Encoding UTF8 | Where-Object { $_ -match 'GPU ' } | Select-Object -Last 1"

echo === 三条路径输出画面的校验值 ===
powershell -NoProfile -Command "Get-FileHash '%OUT_DIR%\traditional.png','%OUT_DIR%\instanced.png','%OUT_DIR%\indirect.png' -Algorithm SHA256 | Format-Table -AutoSize Hash, Path"

exit /b 0
