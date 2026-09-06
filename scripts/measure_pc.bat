@echo off
setlocal

rem PC measurement entry point. Drives a single long-running app process over
rem its TCP control server, one measurement segment per configuration, and the
rem app appends one CSV row per segment end.

set ROOT_DIR=%~dp0..
set EXE=%ROOT_DIR%\build\VulkanIndirectDrawDemo.exe
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\measure_report.csv
set SCRIPT=%ROOT_DIR%\scripts\tcp_send.ps1
set PORT=21000

rem The clock lock is applied to the single process for the whole run
set CLOCK_ARGS=--core-clock 2880 --memory-clock 15001

if not exist "%EXE%" (
    echo Run scripts\build.bat first
    exit /b 1
)
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

start "" /b "%EXE%" --no-interface --control-port %PORT% %CLOCK_ARGS% --report "%REPORT%"

rem Let the process initialize its window and control server
ping -n 4 127.0.0.1 >nul

call :measure 200000 160
call :measure 200000 420
call :measure 1000000 160

call :command "quit" >nul 2>&1

echo Measurement results written to %REPORT%
type "%REPORT%"
exit /b 0

:measure
set P=%1
set F=%2
call :command "path traditional|instances %P%|far %F%|begin"
ping -n 9 127.0.0.1 >nul
call :command "end"
call :command "path instanced|instances %P%|far %F%|begin"
ping -n 9 127.0.0.1 >nul
call :command "end"
call :command "path indirect|instances %P%|far %F%|begin"
ping -n 9 127.0.0.1 >nul
call :command "end"
exit /b 0

:command
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText '%~1'" >nul 2>&1
exit /b 0
