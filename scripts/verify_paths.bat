@echo off
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\meow_indirect_draw.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\verify_report.csv

if not exist "%EXE%" (
    echo Run scripts\build.bat first
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

for %%N in (1000 20000 200000 1000000) do (
    "%EXE%" --instances %%N --far 200 --auto-exit 4 --no-interface --report "%REPORT%"
    "%EXE%" --instances %%N --far 200 --instanced --auto-exit 4 --no-interface --report "%REPORT%"
    "%EXE%" --instances %%N --far 200 --indirect --auto-exit 4 --no-interface --report "%REPORT%"
)

echo Visible instance counts of the three paths written to %REPORT%
type "%REPORT%"
exit /b 0
