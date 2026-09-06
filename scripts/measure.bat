@echo off
setlocal

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\measure_report.csv

if not exist "%EXE%" (
    echo Run scripts\build.bat first
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

rem Every measurement runs at a fixed GPU clock: automatic clock scaling would make the
rem three draw paths incomparable. Drop these two arguments to leave the clocks alone.
set CLOCK_ARGS=--core-clock 2880 --memory-clock 15001

call :measure 200000 160
call :measure 200000 420
call :measure 1000000 160

echo Measurement results written to %REPORT%
type "%REPORT%"
exit /b 0

:measure
"%EXE%" --instances %1 --far %2 %CLOCK_ARGS% --auto-exit 8 --no-interface --report "%REPORT%"
"%EXE%" --instances %1 --far %2 --instanced %CLOCK_ARGS% --auto-exit 8 --no-interface --report "%REPORT%"
"%EXE%" --instances %1 --far %2 --indirect %CLOCK_ARGS% --auto-exit 8 --no-interface --report "%REPORT%"
exit /b 0
