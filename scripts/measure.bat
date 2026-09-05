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

echo === Measuring the timing of both paths with the interface disabled ===
call :measure 200000 160
call :measure 200000 420
call :measure 1000000 160
exit /b 0

:measure
echo --- Instances %1, far plane %2 ---
"%EXE%" --instances %1 --far %2 --auto-exit 6 --no-interface > "%OUT_DIR%\measure_trad.log" 2>&1
powershell -NoProfile -Command "Get-Content '%OUT_DIR%\measure_trad.log' -Encoding UTF8 | Where-Object { $_ -match 'GPU ' } | Select-Object -Last 1"
"%EXE%" --instances %1 --far %2 --indirect --auto-exit 6 --no-interface > "%OUT_DIR%\measure_ind.log" 2>&1
powershell -NoProfile -Command "Get-Content '%OUT_DIR%\measure_ind.log' -Encoding UTF8 | Where-Object { $_ -match 'GPU ' } | Select-Object -Last 1"
exit /b 0
