@echo off
setlocal

rem Android build entry point. Dependency paths come from environment variables:
rem   JAVA_HOME       -> JDK 17 installation (AGP requires 17)
rem   ANDROID_HOME    -> Android SDK root
rem   VULKAN_SDK_DIR  -> optional host Vulkan SDK root, used only to find glslc for
rem                      shader compilation when glslc is not on PATH
rem The build intentionally has no machine specific path in it. When the global
rem environment does not match this machine, place a local_env.bat in this
rem same folder (gitignored) that sets the variables before this script runs,
rem e.g.:
rem     set "JAVA_HOME=D:\path\to\jdk17"
rem     set "ANDROID_HOME=D:\path\to\android-sdk"
rem     set "VULKAN_SDK_DIR=D:\path\to\VulkanSDK"

set ROOT_DIR=%~dp0..
set ANDROID_DIR=%ROOT_DIR%\android
set DEMO_CASE=%~1
if "%DEMO_CASE%"=="" set DEMO_CASE=indirect_draw

if exist "%~dp0local_env.bat" (
    call "%~dp0local_env.bat"
)

if not defined JAVA_HOME (
    echo JAVA_HOME is not set: point it at a JDK 17 installation
    exit /b 1
)
if not exist "%JAVA_HOME%\bin\java.exe" (
    echo JAVA_HOME does not contain a JDK: %JAVA_HOME%
    exit /b 1
)
if not defined ANDROID_HOME (
    echo ANDROID_HOME is not set: point it at the Android SDK
    exit /b 1
)
if not exist "%ANDROID_HOME%\platform-tools" (
    echo ANDROID_HOME does not contain an Android SDK: %ANDROID_HOME%
    exit /b 1
)

pushd "%ANDROID_DIR%"
call gradlew.bat :app:assembleDebug --no-daemon -PdemoCase=%DEMO_CASE%
set RESULT=%ERRORLEVEL%
popd

if not "%RESULT%"=="0" (
    echo Android build failed
    exit /b %RESULT%
)

echo APK written to android\app\build\outputs\apk\debug\app-debug.apk
exit /b 0
