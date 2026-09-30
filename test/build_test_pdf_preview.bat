@echo off
rem build_test_pdf_preview.bat - build & run test\test_pdf_preview.exe (PDF preview backend unit test)
rem precondition: run the repo-root build.bat first so xjs_pdf.obj exists.
rem usage (repo root): MSYS2_ARG_CONV_EXCL='*' cmd /c test\build_test_pdf_preview.bat
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
if not exist "编译中间产物\xjs_pdf.obj" (
    echo ERROR: xjs_pdf.obj missing - run the repo-root build.bat first.
    exit /b 1
)
cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /I. /Fotest\ test\test_pdf_preview.cpp 编译中间产物\xjs_pdf.obj /Fe:test\test_pdf_preview.exe /link ole32.lib windowscodecs.lib uuid.lib user32.lib gdi32.lib
if errorlevel 1 (
    echo ERROR: test compile failed.
    exit /b 1
)
test\test_pdf_preview.exe
exit /b %errorlevel%
