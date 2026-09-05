@echo off
setlocal

set ROOT_DIR=%~dp0..
set ASSET_DIR=%ROOT_DIR%\assets\backpack
set WORK_DIR=%ROOT_DIR%\intermediate

if exist "%ASSET_DIR%\backpack.obj" (
    echo Assets already exist, skipping download
    exit /b 0
)

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if not exist "%ASSET_DIR%" mkdir "%ASSET_DIR%"

if not exist "%WORK_DIR%\backpack.zip" (
    echo Downloading model and textures...
    curl.exe -L -o "%WORK_DIR%\backpack.zip" https://learnopengl.com/data/models/backpack.zip
    if errorlevel 1 exit /b 1
)

powershell -NoProfile -Command "Expand-Archive -Path '%WORK_DIR%\backpack.zip' -DestinationPath '%ASSET_DIR%' -Force"
if errorlevel 1 exit /b 1

echo Assets ready: %ASSET_DIR%
exit /b 0
