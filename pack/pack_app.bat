@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion

rem 定位项目目录，允许用户指定 CMake，否则从 PATH 查找。
for %%I in ("%~dp0..") do set "DESKTOPTOOL_ROOT=%%~fI"
if defined DESKTOPTOOL_CMAKE goto custom_cmake
where cmake.exe >nul 2>&1
if errorlevel 1 goto missing_cmake
set "DESKTOPTOOL_CMAKE=cmake.exe"
goto cmake_ready

:custom_cmake
rem 自定义值只接受可执行文件路径或可由 PATH 找到的文件名。
if exist "%DESKTOPTOOL_CMAKE%" goto cmake_ready
where "%DESKTOPTOOL_CMAKE%" >nul 2>&1
if not errorlevel 1 goto cmake_ready
echo DESKTOPTOOL_CMAKE 指向的程序不存在："%DESKTOPTOOL_CMAKE%"
echo 请设置为 cmake.exe 的完整路径，且不要附加参数。
exit /b 1

:missing_cmake
echo 未找到 CMake。请将 cmake.exe 所在目录加入 PATH，
echo 或设置 DESKTOPTOOL_CMAKE 为 cmake.exe 的完整路径后重试。
exit /b 1

:cmake_ready
rem 从当前发布构建安装完整运行时，保留目标目录中已有的数据。
set "DESKTOPTOOL_BUILD=%DESKTOPTOOL_ROOT%\out\build\web-x64-release"
if not exist "%DESKTOPTOOL_BUILD%\cmake_install.cmake" goto missing_build
if not exist "%DESKTOPTOOL_BUILD%\bin\DesktopTool.exe" goto missing_build
"%DESKTOPTOOL_CMAKE%" --install "%DESKTOPTOOL_BUILD%" --prefix "%DESKTOPTOOL_ROOT%\pack\app"
set "DESKTOPTOOL_EXIT=%ERRORLEVEL%"
if not "%DESKTOPTOOL_EXIT%"=="0" (
    echo 便携发布失败，CMake 错误码：%DESKTOPTOOL_EXIT%。请检查安装输出。
    exit /b %DESKTOPTOOL_EXIT%
)
echo 便携发布完成："%DESKTOPTOOL_ROOT%\pack\app"
exit /b 0

:missing_build
echo 未找到发布构建。请先在项目根目录执行：
echo   cmake --preset x64-release -DCMAKE_PREFIX_PATH="你的 Qt 套件目录"
echo   cmake --build --preset x64-release
exit /b 1
