@echo off
setlocal EnableDelayedExpansion

rem Android device measurement entry point. Runs the same configuration grid as
rem measure_pc.bat (three instance/far groups times three draw paths) against a
rem real phone. Each configuration is a TCP measurement segment: the app listens
rem on the device's loopback control port, "adb forward" maps it to this host,
rem and the rows are collected from the "end" replies into measure_report_android.csv.

set ROOT_DIR=%~dp0..
set OUT_DIR=%ROOT_DIR%\intermediate
set REPORT=%OUT_DIR%\measure_report_android.csv
set SCRIPT=%ROOT_DIR%\scripts\tcp_send.ps1
set PORT=21000

set PACKAGE=com.example.vulkanindirectdrawdemo
set ACTIVITY=android.app.NativeActivity

rem Resolve adb: prefer the Android SDK from local_env.bat when present.
set ADB_EXE=adb
if exist "%~dp0local_env.bat" call "%~dp0local_env.bat"
if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" (
    set "ADB_EXE=%ANDROID_HOME%\platform-tools\adb.exe"
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

rem Write the CSV header once
echo draw_path,instances,visible_instances,draw_commands,frame_ms_avg,frame_ms_stddev,cpu_cull_ms_avg,cpu_cull_ms_stddev,cpu_record_begin_ms_avg,cpu_record_begin_ms_stddev,cpu_record_cull_dispatch_ms_avg,cpu_record_cull_dispatch_ms_stddev,cpu_record_gbuffer_pass_ms_avg,cpu_record_gbuffer_pass_ms_stddev,cpu_record_lighting_pass_ms_avg,cpu_record_lighting_pass_ms_stddev,cpu_record_ui_ms_avg,cpu_record_ui_ms_stddev,cpu_record_capture_ms_avg,cpu_record_capture_ms_stddev,cpu_record_submit_ms_avg,cpu_record_submit_ms_stddev,gpu_ms_avg,gpu_ms_stddev> "%REPORT%"

call :measure 200000 160
call :measure 200000 420
call :measure 1000000 160

"%ADB_EXE%" shell am force-stop %PACKAGE% >nul 2>&1

echo Measurement results written to %REPORT%
type "%REPORT%"
exit /b 0

:measure
set P=%1
set F=%2

call :segment traditional %P% %F%
call :segment instanced %P% %F%
call :segment indirect %P% %F%
exit /b 0

:segment
set SEGPATH=%1
set SEGP=%2
set SEGF=%3

call :ensure_app
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText 'path !SEGPATH!|instances !SEGP!|far !SEGF!|begin'" >nul 2>&1
ping -n 9 127.0.0.1 >nul

set "ROW="
for /f "delims=" %%R in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText 'end'"') do set "ROW=%%R"
set "ROW=!ROW:< row =!"
if not "!ROW!"=="" (
    echo !ROW!>> "%REPORT%"
) else (
    echo [warn] no row returned for path !SEGPATH! instances !SEGP! far !SEGF!
)
exit /b 0

:ensure_app
set "OK="
for /f "delims=" %%R in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText 'lights 64'"') do set "OK=%%R"
if "!OK!"=="< ok" exit /b 0

echo Starting %PACKAGE% on the device...
"%ADB_EXE%" shell am force-stop %PACKAGE% >nul 2>&1
"%ADB_EXE%" forward tcp:%PORT% tcp:%PORT% >nul 2>&1
"%ADB_EXE%" shell am start -S -n %PACKAGE%/%ACTIVITY% >nul 2>&1

for /l %%i in (1,1,30) do (
    set "OK="
    for /f "delims=" %%R in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText 'lights 64'"') do set "OK=%%R"
    if "!OK!"=="< ok" exit /b 0
    ping -n 2 127.0.0.1 >nul
)
echo App did not answer on the control port
exit /b 1
