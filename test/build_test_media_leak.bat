@echo off
rem build_test_media_leak.bat - build & run test\test_media_leak.exe (media engine session loop memory probe)
rem usage (repo root): MSYS2_ARG_CONV_EXCL='*' cmd /c test\build_test_media_leak.bat
rem probe args can follow the bat: cmd /c test\build_test_media_leak.bat <video> [rounds]
rem note: pure ASCII + CRLF.
setlocal
cd /d "%~dp0.."

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do (
    set "VS_PATH=%%i"
    goto :found
)
echo ERROR: Visual Studio not found.
exit /b 1

:found
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvars64 failed.
    exit /b 1
)
cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Fotest\ test\test_media_leak.cpp /Fe:test\test_media_leak.exe
if errorlevel 1 (
    echo ERROR: probe compile failed.
    exit /b 1
)
test\test_media_leak.exe %1 %2
exit /b %errorlevel%
