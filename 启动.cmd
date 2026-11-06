@echo off
setlocal DisableDelayedExpansion
rem Launch the PowerShell script with the same name in this directory.
set "launch_pwsh=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not exist "%launch_pwsh%" set "launch_pwsh=pwsh.exe"

"%launch_pwsh%" -NoProfile -ExecutionPolicy Bypass -File "%~dpn0.ps1" %*
set "launch_exit=%errorlevel%"
if not "%launch_exit%"=="0" (
    echo DesktopTool failed to start. Exit code: %launch_exit%
    pause
)
exit /b %launch_exit%
