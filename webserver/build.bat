@echo off
setlocal
cd /d "%~dp0"

echo Finding Visual Studio...
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do (
    set "VS_PATH=%%i"
    goto :found
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    goto :found
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    goto :found
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    goto :found
)
echo ERROR: Visual Studio not found (VS2017 or later required).
pause
exit /b 1

:found
echo Found Visual Studio: %VS_PATH%
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
del /q obj\*.obj >nul 2>&1

echo Compiling webserver.cpp ...
cl /nologo /EHsc /std:c++20 /O2 /utf-8 /MT /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DNOMINMAX /c webserver.cpp /Foobj\
if errorlevel 1 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)

echo Linking webserver.exe ...
link /nologo /OUT:webserver.exe /MACHINE:X64 /SUBSYSTEM:CONSOLE,6.01 /DEBUG obj\webserver.obj ws2_32.lib
if errorlevel 1 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)
del /q obj\*.obj >nul 2>&1

echo.
echo Build succeeded: webserver.exe
echo Starting server on port %1 (default 8080). Press Ctrl+C here to stop.
echo.
webserver.exe %1
