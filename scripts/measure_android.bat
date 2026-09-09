@echo off
setlocal EnableDelayedExpansion

rem Android device measurement entry point. Runs the same configuration grid as
rem measure_pc.bat (three instance/far groups times three draw paths) against a
rem real phone, with the instance counts reduced to one tenth of the desktop grid
rem because the phone cannot keep up with the desktop loads. Each configuration is
rem a TCP measurement segment: the app listens on the device's loopback control
rem port, "adb forward" maps it to this host, and the rows are collected from the
rem "end" replies into measure_report_android.csv.
rem
rem Usage:
rem   measure_android.bat [serial]
rem The serial selects which connected device the measurement runs on. It is
rem required when more than one device is attached; with a single device
rem attached it can be omitted.

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

rem Pick the target device before anything else so that a missing serial while
rem several devices are attached fails fast without starting a measurement.
set ADB_SERIAL_ARG=%1
call :resolve_device
if errorlevel 1 exit /b 1

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if exist "%REPORT%" del "%REPORT%"

rem Write the CSV header once
echo draw_path,instances,visible_instances,draw_commands,frame_ms_avg,frame_ms_stddev,cpu_cull_ms_avg,cpu_cull_ms_stddev,cpu_record_begin_ms_avg,cpu_record_begin_ms_stddev,cpu_record_cull_dispatch_ms_avg,cpu_record_cull_dispatch_ms_stddev,cpu_record_gbuffer_pass_ms_avg,cpu_record_gbuffer_pass_ms_stddev,cpu_record_lighting_pass_ms_avg,cpu_record_lighting_pass_ms_stddev,cpu_record_ui_ms_avg,cpu_record_ui_ms_stddev,cpu_record_capture_ms_avg,cpu_record_capture_ms_stddev,cpu_record_submit_ms_avg,cpu_record_submit_ms_stddev,gpu_ms_avg,gpu_ms_stddev> "%REPORT%"

rem PC grid divided by ten: 200000/160 -> 20000/160 etc.
call :measure 20000 160
call :measure 20000 420
call :measure 100000 160

"%ADB_EXE%" %ADB_DEVICE% shell am force-stop %PACKAGE% >nul 2>&1

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
"%ADB_EXE%" %ADB_DEVICE% shell am force-stop %PACKAGE% >nul 2>&1
"%ADB_EXE%" %ADB_DEVICE% forward tcp:%PORT% tcp:%PORT% >nul 2>&1
"%ADB_EXE%" %ADB_DEVICE% shell am start -S -n %PACKAGE%/%ACTIVITY% >nul 2>&1

for /l %%i in (1,1,30) do (
    set "OK="
    for /f "delims=" %%R in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT%' -Port %PORT% -CommandsText 'lights 64'"') do set "OK=%%R"
    if "!OK!"=="< ok" exit /b 0
    ping -n 2 127.0.0.1 >nul
)
echo App did not answer on the control port
exit /b 1

:resolve_device
rem Sets ADB_DEVICE to "-s <serial>" for every adb call in this script.
rem ADB_SERIAL_ARG holds the serial from the command line and may be empty.
rem Returns errorlevel 0 on success and 1 when no device is reachable or when
rem several devices are attached but no serial was given.
if "%ADB_SERIAL_ARG%"=="" goto :no_serial_given

set "SERIAL_FOUND="
for /f "tokens=1,2" %%A in ('"%ADB_EXE%" devices') do (
    if "%%B"=="device" (
        if "%%A"=="%ADB_SERIAL_ARG%" set "SERIAL_FOUND=1"
    )
)
if not "%SERIAL_FOUND%"=="1" (
    echo Device %ADB_SERIAL_ARG% is not reachable via adb.
    "%ADB_EXE%" devices
    exit /b 1
)
set "ADB_DEVICE=-s %ADB_SERIAL_ARG%"
exit /b 0

:no_serial_given
set DEVICE_COUNT=0
set "ONLY_SERIAL="
for /f "tokens=1,2" %%A in ('"%ADB_EXE%" devices') do (
    if "%%B"=="device" (
        set /a DEVICE_COUNT+=1
        set "ONLY_SERIAL=%%A"
    )
)
if "%DEVICE_COUNT%"=="0" (
    echo No device is reachable via adb.
    exit /b 1
)
if "%DEVICE_COUNT%"=="1" (
    set "ADB_DEVICE=-s %ONLY_SERIAL%"
    exit /b 0
)
echo Multiple devices are attached to adb. Pass the serial of the target device
echo as the first argument, e.g. "measure_android.bat 0123456789ABCDEF".
"%ADB_EXE%" devices
exit /b 1
