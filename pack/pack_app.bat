@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion

rem 使用 Windows 自带 PowerShell，参数原样交给打包脚本。
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1" %*
set "DESKTOPTOOL_EXIT=%ERRORLEVEL%"
exit /b %DESKTOPTOOL_EXIT%
