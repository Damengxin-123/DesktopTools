@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion

rem ==========================================================
rem  桌面工具 DesktopTool 卸载程序（随安装包部署到安装目录内）
rem ==========================================================

echo ==============================================
echo   桌面工具 DesktopTool 卸载程序
echo ==============================================
echo 安装目录：%~dp0
echo.
set "CONFIRM="
set /p "CONFIRM=确定卸载桌面工具吗？[y/N] "
if /i not "%CONFIRM%"=="y" exit /b 0

tasklist /fi "imagename eq DesktopTool.exe" 2>nul | findstr /i "DesktopTool.exe" >nul
if not errorlevel 1 (
    echo 桌面工具正在运行，请先退出程序（包括托盘图标）再卸载。
    pause
    exit /b 1
)

rem ---- 服务由专用安装器提权卸载，取消授权时保留安装文件 ----
set "DESKTOPTOOL_INDEX_HELPER=%~dp0DesktopToolIndexService.exe"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = Get-Service -Name DesktopToolFileIndex -ErrorAction SilentlyContinue; if ($s) { try { $p = Start-Process -FilePath $env:DESKTOPTOOL_INDEX_HELPER -ArgumentList '--uninstall' -Verb RunAs -WindowStyle Hidden -Wait -PassThru; exit $p.ExitCode } catch { exit 1 } }"
if errorlevel 1 (
    echo 索引服务未能卸载或授权已取消，已停止卸载。
    pause
    exit /b 1
)

rem ---- 删除桌面快捷方式（存在才删，失败不阻塞卸载）----
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = [IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), '桌面工具.lnk'); if ([IO.File]::Exists($p)) { [IO.File]::Delete($p) }" >nul 2>&1

rem ---- 先删除全部子目录（保留用户数据 data），再删除顶层文件 ----
for /d %%D in ("%~dp0*") do if /i not "%%~nxD"=="data" rmdir /s /q "%%~fD"
for %%F in ("%~dp0*") do if /i not "%%~nxF"=="uninstall.bat" del /a /f /q "%%~fF" >nul 2>&1

echo.
set "REMOVE_DATA="
set /p "REMOVE_DATA=是否同时删除用户数据（data 目录）？[y/N] "
if /i "%REMOVE_DATA%"=="y" rmdir /s /q "%~dp0data"

echo 卸载完成。
rem ---- 退出后自删并清理目录；保留 data 时目录非空，删除失败属正常 ----
rem ---- 退出后删除脚本自身并尝试移除空目录；data 保留时目录非空，
rem ---- 非递归 rmdir 自然失败，用户数据不受影响。
(goto) 2>nul & (del /a /f /q "%~f0" & rmdir "%~dp0" & ver >nul)
