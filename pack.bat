@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ============================================================
rem  一键打包脚本
rem  双击运行: 询问打包类型, 输入 1/2/3 回车 (直接回车=完整包)
rem  命令行:   pack.bat [all^|src^|bin] 跳过询问
rem    all = 完整包: 源码与成品, 剔除构建中间物与运行期数据
rem    src = 源码包: 仅源码与构建所需 (SDK/皮肤/语言/词典)
rem    bin = 发行包: 仅运行所需 (exe/引擎/插件/资源)
rem  输出: 仓库根 蜗牛快搜(D2D)_XX包_日期_时间.zip
rem  压缩走系统 .NET ZipFile: 中文条目名 = UTF-8+标志位, 任何解压
rem  软件都不乱码 (系统自带 tar 写中文条目是本机 ANSI 裸字节, 别用)
rem  排除口径与 .gitignore 一致, 需要调整改下面 XD/XF 各行
rem ============================================================

set "MODE=%~1"
set "ASK=1"
if defined MODE set "ASK=0"

if "%ASK%"=="1" (
    echo.
    echo  请选择打包类型:
    echo    1 = 完整包  - 源码与成品, 自动剔除构建垃圾与运行期数据
    echo    2 = 源码包  - 仅源码与构建所需, 不带成品
    echo    3 = 发行包  - 仅运行所需, 发给别人直接用
    echo.
    set "INPUT="
    set /p "INPUT=输入 1/2/3 后回车 [直接回车=1 完整包]: "
    if not defined INPUT set "INPUT=1"
    if "!INPUT!"=="1" set "MODE=all"
    if "!INPUT!"=="2" set "MODE=src"
    if "!INPUT!"=="3" set "MODE=bin"
    if not defined MODE if /i "!INPUT!"=="all" set "MODE=all"
    if not defined MODE if /i "!INPUT!"=="src" set "MODE=src"
    if not defined MODE if /i "!INPUT!"=="bin" set "MODE=bin"
    if not defined MODE (
        echo [输入无效] 只认 1/2/3 或 all/src/bin
        pause
        exit /b 1
    )
)

if /i "%MODE%"=="all" set "TAG=完整包"
if /i "%MODE%"=="src" set "TAG=源码包"
if /i "%MODE%"=="bin" set "TAG=发行包"
if not defined TAG (
    echo [参数错误] 用法: pack.bat [all^|src^|bin]
    if "%ASK%"=="1" pause
    exit /b 1
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmm"') do set "TS=%%i"
if not defined TS set "TS=0"
set "OUT=蜗牛快搜(D2D)_!TAG!_%TS%.zip"
if exist "!OUT!" del /f /q "!OUT!"
set "TMPD=%TEMP%\snailpack_%TS%"
rd /s /q "!TMPD!" 2>nul

rem ---- 目录排除: 构建中间物 / 运行期数据 / 本地协作目录 ----
rem 注意: /XD 只认裸目录名(任意层级匹配), 带路径写法不生效
set "XD=/XD .git .zcode .zcodetmp .vscode 编译中间产物 dumps data webview2-data SSL obj"

rem ---- 文件排除: 产物扩展名 / 运行期文件 / 本地协作文件 ----
set "XF=/XF xjs_db.dat xjs_config.json startup_stack.txt xunjieso_*捕获崩溃*.txt"
set "XF=!XF! *.obj *.res *.ilk *.pdb *.exp *.log probe2.lib ai_core.lib"
set "XF=!XF! *.7z *.zip"
set "XF=!XF! AGENTS.md .gitignore bg_*.sh release_body.json sponsor-alipay.jpg sponsor-wechat.png"
set "XF=!XF! _cdb_*.txt _diag_*.* _probe*.txt webserver.exe space_map.lib"

rem /R:1 /W:1 = 遇被锁文件重试 1 次即跳过, 防默认策略(重试百万次)挂死
set "OPT=/R:1 /W:1"

if /i "%MODE%"=="bin" goto :packbin

if /i "%MODE%"=="src" (
    set "XD=!XD! plugins"
    set "XF=!XF! SnailQuickSearch.exe ai-assistant.dll"
)
robocopy . "!TMPD!" /E %OPT% %XD% %XF% >nul
if errorlevel 8 goto :fail
goto :dozip

:packbin
rem ---- 发行包: 白名单, 只挑运行所需 ----
robocopy . "!TMPD!" %OPT% SnailQuickSearch.exe SnailQuickSearch.exe.manifest snailsearch.ico xunjieso.dll donate-alipay.jpg donate-wechat.png README.md LICENSE.md THIRD-PARTY-NOTICES.md >nul
if errorlevel 8 goto :fail
robocopy languages "!TMPD!\languages" /E %OPT% >nul
if errorlevel 8 goto :fail
robocopy skin "!TMPD!\skin" /E %OPT% >nul
if errorlevel 8 goto :fail
robocopy Config "!TMPD!\Config" %OPT% Alias.json Filter.json >nul
if errorlevel 8 goto :fail
robocopy plugins "!TMPD!\plugins" /E %OPT% /XD data webview2-data >nul
if errorlevel 8 goto :fail

:dozip
rem 逐文件写入: 条目名用 / 分隔 (CreateFromDirectory 会写 \ 分隔, 跨平台不兼容)
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z=[System.IO.Compression.ZipFile]::Open('!CD!\!OUT!','Create'); Get-ChildItem -LiteralPath '!TMPD!' -Recurse -File | ForEach-Object { $r=$_.FullName.Substring(('!TMPD!').Length+1) -replace '\\','/'; [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $_.FullName, $r, 'Optimal') | Out-Null }; $z.Dispose()" >nul 2>&1
rd /s /q "!TMPD!" 2>nul
if errorlevel 1 goto :fail
if not exist "!OUT!" goto :fail
for %%F in ("!OUT!") do set /a FMB=%%~zF/1048576
echo [完成] !OUT!  共 !FMB! MB
if "%ASK%"=="1" pause
endlocal
exit /b 0

:fail
rd /s /q "!TMPD!" 2>nul
echo [失败] 打包未完成, 请重试或检查磁盘与权限
if "%ASK%"=="1" pause
exit /b 1
