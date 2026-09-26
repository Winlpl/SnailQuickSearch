@echo off
setlocal
cd /d "%~dp0"

echo Building ai-assistant plugin (WebView2 UI) ...

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

:: SDK headers live in the repo root (two levels up from this folder)
if not exist "..\..\xjs_plugin_sdk.h" (
    echo ERROR: xjs_plugin_sdk.h not found at ..\..\  - keep this sample inside the repo.
    pause
    exit /b 1
)

:: WebView2 SDK lives in the repo root too. The loader is the official
:: WebView2Loader.dll (dynamically loaded, deployed next to the plugin DLL) --
:: NOT the static lib, whose embedded CRT objects corrupted the environment
:: pointer delivered by the creation callback (crash on panel open).
if not exist "..\..\webview2\include\WebView2.h" (
    echo ERROR: WebView2 SDK not found at ..\..\webview2 - keep it inside the repo.
    pause
    exit /b 1
)
if not exist "..\..\webview2\x64\WebView2Loader.dll" (
    echo ERROR: WebView2Loader.dll not found at ..\..\webview2\x64 - keep it inside the repo.
    pause
    exit /b 1
)

:: UI = embedded WebView2 frontend (system Edge runtime renders; no GDI+ bitmap
:: pipeline anymore). md4c is compiled straight in (markdown -> HTML for bubbles).
:: WinToast is compiled straight in too (system toast notifications, no PowerShell).
cl /nologo /EHsc /std:c++20 /O2 /MP /Zi /Fd:ai-assistant.pdb /utf-8 /MT /DUNICODE /D_UNICODE /LD ^
   /I..\..\webview2\include ^
   ai_core.cpp ai_agent.cpp ai_session.cpp ai_web.cpp ai_web_ui.cpp ai_plugin.cpp ai_file.cpp ^
   ..\..\md4c\md4c.c ^
   ..\..\WinToast\wintoastlib.cpp ^
   /link /OUT:ai-assistant.dll winhttp.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib gdiplus.lib windowscodecs.lib propsys.lib runtimeobject.lib ^
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

:: Deploy to the runtime plugins dir (exe dir \ plugins \ ai-assistant)
if not exist "..\..\plugins\ai-assistant" mkdir "..\..\plugins\ai-assistant"
copy /y ai-assistant.dll "..\..\plugins\ai-assistant\ai-assistant.dll" >nul
if errorlevel 1 (
    echo ERROR: deploy failed - plugins\ai-assistant\ai-assistant.dll is locked.
    pause
    exit /b 1
)
copy /y manifest.json "..\..\plugins\ai-assistant\manifest.json" >nul
copy /y "..\..\webview2\x64\WebView2Loader.dll" "..\..\plugins\ai-assistant\WebView2Loader.dll" >nul
if errorlevel 1 (
    echo ERROR: deploy failed - WebView2Loader.dll is locked.
    pause
    exit /b 1
)
echo.
echo Build succeeded: ai-assistant.dll
echo Deployed to plugins\ai-assistant\  (enable it in Settings - Plugins)
pause
