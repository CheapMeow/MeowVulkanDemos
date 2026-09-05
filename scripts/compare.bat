@echo off
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\compare_report.csv
set HASH_REPORT=%OUT_DIR%\compare_hashes.csv

if not exist "%EXE%" (
    echo Run scripts\build.bat first
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

set INSTANCES=%1
if "%INSTANCES%"=="" set INSTANCES=200000
set FAR=%2
if "%FAR%"=="" set FAR=160
set SECONDS=%3
if "%SECONDS%"=="" set SECONDS=8

"%EXE%" --instances %INSTANCES% --far %FAR% --auto-exit %SECONDS% --no-interface --report "%REPORT%" --capture "%OUT_DIR%\traditional.png"
"%EXE%" --instances %INSTANCES% --far %FAR% --instanced --auto-exit %SECONDS% --no-interface --report "%REPORT%" --capture "%OUT_DIR%\instanced.png"
"%EXE%" --instances %INSTANCES% --far %FAR% --indirect --auto-exit %SECONDS% --no-interface --report "%REPORT%" --capture "%OUT_DIR%\indirect.png"

powershell -NoProfile -Command "Get-FileHash '%OUT_DIR%\traditional.png','%OUT_DIR%\instanced.png','%OUT_DIR%\indirect.png' -Algorithm SHA256 | Select-Object @{Name='file';Expression={Split-Path $_.Path -Leaf}}, @{Name='sha256';Expression={$_.Hash}} | Export-Csv -Path '%HASH_REPORT%' -NoTypeInformation -Encoding UTF8"

echo Timing results:
type "%REPORT%"
echo.
echo Captured image checksums:
type "%HASH_REPORT%"
exit /b 0
