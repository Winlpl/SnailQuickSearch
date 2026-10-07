@echo off
setlocal
cd /d "%~dp0"

echo Building sql-generator plugin (GDI+ custom-draw app window) ...

:: Find Visual Studio (same locate order as the host build.bat)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do (
    set "VS_PATH=%%i"
    goto :found
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community" & goto :found
echo ERROR: Visual Studio not found.
pause
exit /b 1

:found
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:: SDK header lives in the repo root (two levels up from this folder)
if not exist "..\..\xjs_plugin_sdk.h" (
    echo ERROR: xjs_plugin_sdk.h not found at ..\..\  - keep this sample inside the repo.
    pause
    exit /b 1
)
:: Engine straight-link (same policy as ai-assistant): include xunjieso.h, link the
:: import lib at the repo root - the loader binds to the instance the host loaded.
if not exist "..\..\xunjieso.lib" (
    echo ERROR: xunjieso.lib not found at ..\..\  - the engine SDK files live in the repo root.
    pause
    exit /b 1
)

:: Language packs (Sg section of the original language files) = RCDATA 301..305.
if not exist "lang\zh-CN.json" (
    echo ERROR: lang\zh-CN.json not found - language packs are part of the sources.
    pause
    exit /b 1
)
rc /nologo /fo sqlgen_ui.res sqlgen_ui.rc
if errorlevel 1 (
    echo ERROR: resource compile failed - sqlgen_ui.rc.
    pause
    exit /b 1
)

cl /nologo /EHsc /std:c++20 /O2 /MP /Zi /Fd:sql-generator.pdb /utf-8 /MT /DUNICODE /D_UNICODE /LD ^
   sql_generator.cpp ^
   sqlgen_ui.res ^
   /link /OUT:sql-generator.dll user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib gdiplus.lib dwmapi.lib imm32.lib shlwapi.lib ^
   ..\..\xunjieso.lib
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

:: A running instance locks the deployed DLL (copy fails): terminate it before
:: deploying. Same policy as the host build.bat - never block on a running
:: instance. System32 absolute paths: a bare `find` would hit GNU find.
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq SnailQuickSearch.exe" 2>nul | %SystemRoot%\System32\find.exe /I "SnailQuickSearch.exe" >nul
if %errorlevel% neq 0 goto :nokill
echo Terminating running SnailQuickSearch.exe ...
%SystemRoot%\System32\taskkill.exe /F /IM SnailQuickSearch.exe >nul 2>&1
set /a KILL_TRIES=0
:waitkill
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq SnailQuickSearch.exe" 2>nul | %SystemRoot%\System32\find.exe /I "SnailQuickSearch.exe" >nul
if %errorlevel% neq 0 goto :nokill
set /a KILL_TRIES+=1
if %KILL_TRIES% lss 3 goto :waitkill
echo Instance survives (elevated). Requesting elevated kill (UAC) ...
powershell -NoProfile -Command "Start-Process -FilePath '%SystemRoot%\System32\taskkill.exe' -ArgumentList '/F','/IM','SnailQuickSearch.exe' -Verb RunAs -Wait" >nul 2>&1
set /a KILL_TRIES=0
:waitkill2
%SystemRoot%\System32\ping.exe -n 2 127.0.0.1 >nul
%SystemRoot%\System32\tasklist.exe /FI "IMAGENAME eq SnailQuickSearch.exe" 2>nul | %SystemRoot%\System32\find.exe /I "SnailQuickSearch.exe" >nul
if %errorlevel% neq 0 goto :nokill
set /a KILL_TRIES+=1
if %KILL_TRIES% lss 3 goto :waitkill2
echo ERROR: SnailQuickSearch.exe could not be terminated (UAC declined?).
pause
exit /b 1
:nokill

:: Deploy to the runtime plugins dir (exe dir \ plugins \ sql-generator)
if not exist "..\..\plugins\sql-generator" mkdir "..\..\plugins\sql-generator"
copy /y sql-generator.dll "..\..\plugins\sql-generator\sql-generator.dll" >nul
if errorlevel 1 (
    echo ERROR: deploy failed - plugins\sql-generator\sql-generator.dll is locked.
    pause
    exit /b 1
)
copy /y manifest.json "..\..\plugins\sql-generator\manifest.json" >nul
copy /y icon.png "..\..\plugins\sql-generator\icon.png" >nul
copy /y icon.svg "..\..\plugins\sql-generator\icon.svg" >nul
echo.
echo Build succeeded: sql-generator.dll
echo Deployed to plugins\sql-generator\  (enable it in Settings - Plugins)
pause
