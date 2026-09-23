@echo off
setlocal

:: Always work from this script's folder (rc/cl need the sources in CWD)
cd /d "%~dp0"

echo Finding Visual Studio...

:: Method 1: Use vswhere for VS 2022/2019/2017
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do (
    set "VS_PATH=%%i"
    goto :found
)

:: Method 2: Find VS 2022
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

:: Method 3: Find VS 2019
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
    goto :found
)
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
    goto :found
)

:: Method 4: Find VS 2017
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community"
    goto :found
)

echo ERROR: Visual Studio not found. Please install VS 2017 or later.
pause
exit /b 1

:found
echo Found Visual Studio: %VS_PATH%
echo.

:: Setup VS environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:: A running instance locks the exe (LNK1104): terminate it and wait for the lock to
:: release, then build. The app self-elevates (UAC) at startup, so a non-admin shell
:: cannot kill it -> fall back to an elevated taskkill via UAC (silent when UAC =
:: "never notify"). Force-kill skips the DB flush -> next start does a full rescan
:: (accepted trade-off: the build must never block on a running instance).
:: System32 absolute paths: a bare `find` would hit GNU find from Git Bash's PATH
:: (inherited by cmd) and break every check below.
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

:: All intermediate files (obj / res / compiler pdb) go into this folder, exe stays in repo root.
:: NOTE: this file is saved in ANSI (GBK) encoding because the folder name is Chinese - keep it that way.
set "INT_DIR=编译中间产物"
if not exist "%INT_DIR%" mkdir "%INT_DIR%"

:: ==================== full build every run (incremental logic removed) ====================
:: The per-TU timestamp checks kept missing same-minute edits and mixing old/new
:: objects, so every run now rebuilds everything. Wipe old .obj first so the
:: wildcard link can never pick up orphans of renamed TUs.
del /q "%INT_DIR%\*.obj" >nul 2>&1

echo Compiling resources...
rc /fo %INT_DIR%\main.res main.rc
if errorlevel 1 (
    echo ERROR: Resource compilation failed!
    echo Please ensure main.rc, snailsearch.ico and the embedded documents exist.
    pause
    exit /b 1
)

set "SRCS=xjs_app.cpp xjs_util.cpp xjs_d2d.cpp xjs_gdiplus.cpp xjs_engine.cpp xjs_popup.cpp xjs_chrome.cpp xjs_toast.cpp xjs_list.cpp xjs_preview.cpp xjs_settings.cpp xjs_md.cpp xjs_plugin.cpp xjs_plugin_api.cpp md4c\md4c.c main.cpp"
echo Compiling all sources /MP parallel...
cl /MP /EHsc /std:c++20 /O2 /GL /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0601 /c %SRCS% /Fo%INT_DIR%\ /Fd%INT_DIR%\
if errorlevel 1 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)

echo Linking SnailQuickSearch.exe ...
link /OUT:SnailQuickSearch.exe /MACHINE:X64 /SUBSYSTEM:WINDOWS,6.01 /LTCG /DEBUG /OPT:REF /OPT:ICF /MANIFEST:EMBED "%INT_DIR%\*.obj" "%INT_DIR%\main.res" xunjieso.lib comctl32.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib imm32.lib oleaut32.lib shlwapi.lib uuid.lib
if errorlevel 1 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)
echo.
echo Build succeeded!
echo Output: SnailQuickSearch.exe
echo.
echo Press any key to run...
pause >nul
start SnailQuickSearch.exe
exit /b 0
