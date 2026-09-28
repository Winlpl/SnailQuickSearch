@echo off
rem build_test_ai_net.bat — 编译并运行 test\test_ai_net.exe (联网层纯解析单测)
rem 前置: 先跑过 插件示例\ai-assistant\build.bat 让 ai_core.obj / ai_file.obj / ai_net.obj 在位。
rem 用法 (仓库根): MSYS2_ARG_CONV_EXCL='*' cmd /c test\build_test_ai_net.bat
rem 注意: 本文件含中文路径, 必须 GBK(ANSI) 编码 + CRLF 行尾。
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Fotest\ /I插件示例\ai-assistant test\test_ai_net.cpp 插件示例\ai-assistant\ai_core.obj 插件示例\ai-assistant\ai_file.obj 插件示例\ai-assistant\ai_net.obj /Fe:test\test_ai_net.exe /link winhttp.lib ws2_32.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib gdiplus.lib windowscodecs.lib propsys.lib runtimeobject.lib xunjieso.lib
if errorlevel 1 (echo NET-TEST-BUILD-FAILED
exit /b 1)
test\test_ai_net.exe
