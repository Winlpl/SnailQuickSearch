@echo off
rem build_test_ai_file.bat — 编译并运行 test\test_ai_file.exe (read_file/AiTextToUtf8 纯逻辑单测)
rem 前置: 先跑过 插件示例\ai-assistant\build.bat 让 ai_core.obj / ai_file.obj 在位。
rem 用法 (仓库根): MSYS2_ARG_CONV_EXCL='*' cmd /c test\build_test_ai_file.bat
rem 注意: 本文件含中文路径, 必须 GBK(ANSI) 编码 + CRLF 行尾。
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
if not exist "插件示例\ai-assistant\ai_core.obj" (
    echo ERROR: 插件示例\ai-assistant\ai_core.obj missing - run the plugin build.bat first.
    exit /b 1
)
cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Fotest\ /I插件示例\ai-assistant test\test_ai_file.cpp 插件示例\ai-assistant\ai_core.obj 插件示例\ai-assistant\ai_file.obj /Fe:test\test_ai_file.exe /link winhttp.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib gdiplus.lib windowscodecs.lib propsys.lib runtimeobject.lib xunjieso.lib
if errorlevel 1 (
    echo ERROR: test compile failed.
    exit /b 1
)
test\test_ai_file.exe
exit /b %errorlevel%
