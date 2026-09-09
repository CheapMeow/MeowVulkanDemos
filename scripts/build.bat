@echo off
setlocal

rem Desktop build entry point. Dependency paths come from environment variables:
rem   VS_DIR          -> Visual Studio installation root. The build uses the CMake
rem                      and Ninja bundled with it and the MSVC toolchain through
rem                      vcvars64.bat.
rem   VULKAN_SDK_DIR  -> Vulkan SDK root (headers, vulkan-1.lib, glslc).
rem This file contains no machine specific path. When the global environment does
rem not match this machine, place a local_env.bat in this same folder (gitignored)
rem that sets the variables before this script runs, e.g.:
rem     set "VS_DIR=D:\path\to\Visual Studio\2019\Community"
rem     set "VULKAN_SDK_DIR=D:\path\to\VulkanSDK"

set ROOT_DIR=%~dp0..

if exist "%~dp0local_env.bat" (
    call "%~dp0local_env.bat"
)

if not defined VS_DIR (
    echo VS_DIR is not set: point it at a Visual Studio installation
    exit /b 1
)

set VCVARS=%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo VS_DIR does not contain a Visual Studio installation: %VS_DIR%
    exit /b 1
)

set CMAKE_EXE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA_EXE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

if not exist "%ROOT_DIR%\build" (
    "%CMAKE_EXE%" -S "%ROOT_DIR%" -B "%ROOT_DIR%\build" -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
        -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    if errorlevel 1 exit /b 1
)

"%CMAKE_EXE%" --build "%ROOT_DIR%\build"
exit /b %errorlevel%
