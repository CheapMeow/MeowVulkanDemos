@echo off
setlocal

rem RenderDoc Android capture launcher. Mirrors how the tooling in
rem PerformanceTools drives org.renderdoc.renderdoccmd.arm64 over adb:
rem the renderdoc remote server is started on the device, the Vulkan capture
rem layer is attached to the target package through the global GPU debug
rem layer settings, and the app is then started under the layer. Nothing runs
rem on this PC besides adb.
rem
rem Usage:
rem   renderdoc_capture.bat start [serial]
rem   renderdoc_capture.bat stop  [serial]
rem
rem Capture files land on the device under
rem   /sdcard/Android/media/<package>/files/RenderDoc/
rem and are pulled from there with adb.

set PACKAGE=com.example.vulkanindirectdrawdemo
set ACTIVITY=android.app.NativeActivity
set RDC_PACKAGE=org.renderdoc.renderdoccmd.arm64
set CAPOPTS=ababaaaaaaaaaaaaafaaaaaaaaabaaaaabaaaaaaaaaaaaaa

rem Resolve adb: prefer the Android SDK from local_env.bat when present.
set ADB_EXE=adb
if exist "%~dp0local_env.bat" call "%~dp0local_env.bat"
if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" (
    set "ADB_EXE=%ANDROID_HOME%\platform-tools\adb.exe"
)

set MODE=%1
if "%MODE%"=="" set MODE=start

set ADB_DEVICE=
if not "%2"=="" set "ADB_DEVICE=-s %2"

if not "%MODE%"=="start" if not "%MODE%"=="stop" (
    echo Unknown mode %MODE%. Use start or stop.
    exit /b 1
)

"%ADB_EXE%" %ADB_DEVICE% get-state >nul 2>&1
if errorlevel 1 (
    echo No device is reachable via adb.
    exit /b 1
)

if "%MODE%"=="start" (
    echo Starting RenderDoc remote server on the device...
    "%ADB_EXE%" %ADB_DEVICE% shell am force-stop %RDC_PACKAGE%
    "%ADB_EXE%" %ADB_DEVICE% shell am start -n %RDC_PACKAGE%/.Loader -e "renderdoccmd remoteserver"

    echo Attaching the RenderDoc capture layer to %PACKAGE%...
    "%ADB_EXE%" %ADB_DEVICE% shell am force-stop %PACKAGE%
    "%ADB_EXE%" %ADB_DEVICE% shell settings put global enable_gpu_debug_layers 1
    "%ADB_EXE%" %ADB_DEVICE% shell settings put global gpu_debug_app %PACKAGE%
    "%ADB_EXE%" %ADB_DEVICE% shell settings put global gpu_debug_layer_app %RDC_PACKAGE%
    "%ADB_EXE%" %ADB_DEVICE% shell settings put global gpu_debug_layers VK_LAYER_RENDERDOC_Capture
    "%ADB_EXE%" %ADB_DEVICE% shell settings put global gpu_debug_layers_gles libVkLayer_GLES_RenderDoc.so
    "%ADB_EXE%" %ADB_DEVICE% shell setprop debug.rdoc.IGNORE_LAYERS 0
    "%ADB_EXE%" %ADB_DEVICE% shell setprop debug.vr.profiler 1
    "%ADB_EXE%" %ADB_DEVICE% shell mkdir -p /sdcard/Android/media/%PACKAGE%/files
    "%ADB_EXE%" %ADB_DEVICE% shell setprop debug.rdoc.RENDERDOC_CAPOPTS %CAPOPTS%

    "%ADB_EXE%" %ADB_DEVICE% shell pm path %PACKAGE%
    if errorlevel 1 (
        echo %PACKAGE% is not installed on the device.
    )

    echo Launching %PACKAGE% under the capture layer...
    "%ADB_EXE%" %ADB_DEVICE% shell am start -S -n %PACKAGE%/%ACTIVITY%

    echo.
    echo RenderDoc capture armed. Captures are written on the device under
    echo /sdcard/Android/media/%PACKAGE%/files/RenderDoc/
    exit /b 0
)

rem stop mode: detach the layer and reset the device state
echo Detaching the RenderDoc capture layer from %PACKAGE%...
"%ADB_EXE%" %ADB_DEVICE% shell am force-stop %PACKAGE%
"%ADB_EXE%" %ADB_DEVICE% shell settings put global enable_gpu_debug_layers 0
rem adb drops empty string arguments, so clear the keys by deleting them
"%ADB_EXE%" %ADB_DEVICE% shell settings delete global gpu_debug_app
"%ADB_EXE%" %ADB_DEVICE% shell settings delete global gpu_debug_layer_app
"%ADB_EXE%" %ADB_DEVICE% shell settings delete global gpu_debug_layers
"%ADB_EXE%" %ADB_DEVICE% shell settings delete global gpu_debug_layers_gles
echo RenderDoc layer detached.
exit /b 0
