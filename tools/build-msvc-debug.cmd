@echo off
setlocal

set "VSDEVCMD=D:\11AndroidLearnings\Env\Visual Studio 2026\Common7\Tools\VsDevCmd.bat"
set "CMAKE_EXE=D:\11AndroidLearnings\Env\Visual Studio 2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_DIR=D:\11AndroidLearnings\Env\Visual Studio 2026\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "SOURCE_DIR=%~dp0.."
set "BUILD_DIR=%SOURCE_DIR%\build\msvc2026_64-Debug"

if not exist "%VSDEVCMD%" (
    echo Visual Studio developer environment was not found: %VSDEVCMD%
    exit /b 1
)
if not exist "%CMAKE_EXE%" (
    echo CMake was not found: %CMAKE_EXE%
    exit /b 1
)
if not exist "%NINJA_DIR%\ninja.exe" (
    echo Ninja was not found: %NINJA_DIR%\ninja.exe
    exit /b 1
)

call "%VSDEVCMD%" -arch=x64
if errorlevel 1 exit /b %errorlevel%

set "PATH=%NINJA_DIR%;%PATH%"

"%CMAKE_EXE%" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_PREFIX_PATH=G:/Dev/Qt/6.8.3/msvc2022_64
if errorlevel 1 exit /b %errorlevel%

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Debug -j 4
exit /b %errorlevel%
