@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion

rem ==========================================================
rem  桌面工具 DesktopTool @VERSION@ 安装程序（由打包脚本生成）
rem  用法：双击交互安装；或 install.bat [安装位置的父目录] 静默安装
rem ==========================================================

set "PAYLOAD_DIR=@PAYLOAD_DIR@"
set "APP_DIR_NAME=DesktopTool"
set "SILENT="
if not "%~1"=="" set "SILENT=1"

rem ---- 程序文件必须与本脚本在同一层（解压后的压缩包根目录）----
if not exist "%~dp0%PAYLOAD_DIR%\DesktopTool.exe" (
    echo 未找到程序文件：%~dp0%PAYLOAD_DIR%
    echo 请先将整个压缩包解压到任意目录，再运行 install.bat。
    goto fail
)

echo ==============================================
echo   桌面工具 DesktopTool @VERSION@ 安装程序
echo ==============================================
echo.

rem ---- 运行中的程序会锁定文件，导致复制失败 ----
tasklist /fi "imagename eq DesktopTool.exe" 2>nul | findstr /i "DesktopTool.exe" >nul
if not errorlevel 1 (
    echo 检测到桌面工具正在运行，请先退出程序（包括托盘图标）再安装。
    goto fail
)

rem ---- 选择安装位置；安装程序会在其中创建 DesktopTool 子目录 ----
if defined SILENT (
    set "USER_BASE=%~1"
    goto sanitize_base
)
set "DEFAULT_BASE=%LOCALAPPDATA%\Programs"
echo 安装程序会在你指定的位置下创建 %APP_DIR_NAME% 子目录，
echo 程序与数据都保存在该子目录内，升级安装会原样保留数据。
echo 直接回车使用默认位置：%DEFAULT_BASE%
set "USER_BASE="
set /p "USER_BASE=安装位置（例如 J:\TOOL）: "
if not defined USER_BASE set "USER_BASE=%DEFAULT_BASE%"

:sanitize_base
rem ---- 去除输入中的引号（路径含空格直接输入即可，无需引号）----
setlocal EnableDelayedExpansion
set "BASE_DIR=!USER_BASE:"=!"
endlocal & set "USER_BASE=%BASE_DIR%"
set "BASE_DIR=%USER_BASE%"

rem ---- 必须是包含盘符的绝对路径 ----
if "%BASE_DIR%"=="" goto invalid_base
if "%BASE_DIR::=%"=="%BASE_DIR%" goto invalid_base

rem ---- 规范化结尾：保留盘符根的反斜杠，其余去掉 ----
if "%BASE_DIR:~-2%"==":\" goto base_ready
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
:base_ready
if "%BASE_DIR:~-1%"=="\" (set "TARGET=%BASE_DIR%%APP_DIR_NAME%") else set "TARGET=%BASE_DIR%\%APP_DIR_NAME%"

echo.
echo 安装目标：%TARGET%
if defined SILENT goto confirm_ready
set "CONFIRM="
set /p "CONFIRM=确认安装到上述位置？[Y/n] "
if /i "%CONFIRM%"=="n" goto cancelled
:confirm_ready

rem ---- 创建目标目录并检查可写性 ----
if not exist "%TARGET%\" (
    mkdir "%TARGET%" 2>nul
    if not exist "%TARGET%\" (
        echo 无法创建目录：%TARGET%
        goto fail
    )
)
copy /y nul "%TARGET%\.write-test" >nul 2>&1
if not exist "%TARGET%\.write-test" (
    echo 目标目录无法写入：%TARGET%
    echo 请检查磁盘空间与权限后重试。
    goto fail
)
del /a /f /q "%TARGET%\.write-test" >nul 2>&1

rem ---- 复制程序与资源；升级安装只覆盖程序，data 用户数据原样保留 ----
echo 正在复制程序文件…
robocopy "%~dp0%PAYLOAD_DIR%" "%TARGET%" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /NFL /NDL /NP >nul
if errorlevel 8 (
    echo 程序文件复制失败，请确认磁盘空间与文件占用后重试。
    goto fail
)

rem ---- 桌面快捷方式（静默安装时跳过）----
if defined SILENT goto shortcut_done
set "MAKE_SHORTCUT=y"
set /p "MAKE_SHORTCUT=是否创建桌面快捷方式？[Y/n] "
if /i "%MAKE_SHORTCUT%"=="n" goto shortcut_done
set "DESKTOPTOOL_TARGET=%TARGET%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$w = New-Object -ComObject WScript.Shell; $t = $env:DESKTOPTOOL_TARGET; $s = $w.CreateShortcut([IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), '桌面工具.lnk')); $s.TargetPath = Join-Path $t 'DesktopTool.exe'; $s.WorkingDirectory = $t; $s.Save()" >nul 2>&1
if errorlevel 1 echo 快捷方式创建失败，不影响使用，可手动发送到桌面。
:shortcut_done

echo.
echo ==============================================
echo   安装完成：%TARGET%
echo ==============================================
echo 双击其中的 DesktopTool.exe 即可使用；
echo 卸载请运行该目录内的 uninstall.bat。
if not defined SILENT pause
exit /b 0

:invalid_base
echo 安装位置无效："%BASE_DIR%"
echo 请输入包含盘符的绝对路径，例如 J:\TOOL。
goto fail

:cancelled
echo 已取消安装。
goto fail

:fail
if defined SILENT exit /b 1
echo.
echo 按任意键退出…
pause >nul
exit /b 1
