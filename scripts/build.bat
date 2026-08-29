@echo off
setlocal

set VS_DIR=D:\path\to\Visual Studio\2019\Community
set CMAKE_EXE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA_EXE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set ROOT_DIR=%~dp0..

call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "%ROOT_DIR%\build" (
    "%CMAKE_EXE%" -S "%ROOT_DIR%" -B "%ROOT_DIR%\build" -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if errorlevel 1 exit /b 1
)

"%CMAKE_EXE%" --build "%ROOT_DIR%\build"
exit /b %errorlevel%
