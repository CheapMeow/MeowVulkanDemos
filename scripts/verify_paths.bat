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

echo === 不同实例数量下两条路径的可见实例数量与耗时 ===
for %%N in (1000 20000 200000 1000000) do (
    "%EXE%" --instances %%N --far 200 --auto-exit 4 --no-interface > "%OUT_DIR%\trad_%%N.log" 2>&1
    "%EXE%" --instances %%N --far 200 --indirect --auto-exit 4 --no-interface > "%OUT_DIR%\ind_%%N.log" 2>&1
    echo --- 实例 %%N ---
    powershell -NoProfile -Command "Get-Content '%OUT_DIR%\trad_%%N.log' -Encoding UTF8 | Where-Object { $_ -match '绘制命令' } | Select-Object -Last 1"
    powershell -NoProfile -Command "Get-Content '%OUT_DIR%\ind_%%N.log' -Encoding UTF8 | Where-Object { $_ -match '绘制命令' } | Select-Object -Last 1"
)

exit /b 0
