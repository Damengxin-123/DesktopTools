# 启动独立 Godot 工程，按需构建或重新部署 C++ 后台。
[CmdletBinding()]
param(
    [switch]$Editor,
    [switch]$Rebuild,
    # 仅构建和部署后台，供验证及自动化打包使用。
    [switch]$BuildOnly,
    [string]$DataDirectory,
    [string]$GodotExe = 'I:\PROJECT\Godot\Puppet\tools\Godot\Godot_v4.7.2-stable_win64.exe',
    [string]$QtRoot = 'I:\TOOL\Qt\6.10.3\msvc2022_64',
    [string]$CMake = 'I:\TOOL\Qt\Tools\CMake_64\bin\cmake.exe',
    [string]$Ninja = 'I:\TOOL\Qt\Tools\Ninja\ninja.exe',
    [string]$VsShell = 'I:\TOOL\VisualStudio2026\Common7\Tools\Launch-VsDevShell.ps1',
    [string]$NativeDeps,
    [string]$BoostConfig,
    [string]$OpenSsl = 'I:\TOOL\Python\Miniforge3\pkgs\openssl-3.6.3-hf411b9b_0\Library'
)

$ErrorActionPreference = 'Stop'
$runtime = Join-Path $PSScriptRoot 'native/runtime'
$backend = Join-Path $runtime 'DesktopToolNative.exe'
if (-not (Test-Path -LiteralPath $GodotExe -PathType Leaf)) {
    throw '未找到 Godot，请使用 -GodotExe 指定 Godot 4.6 或更新版本。'
}
if ($Rebuild -or -not (Test-Path -LiteralPath $backend -PathType Leaf)) {
    if (-not $NativeDeps) { $NativeDeps = Join-Path $PSScriptRoot 'native/dependencies' }
    if (-not $BoostConfig) { $BoostConfig = Join-Path $NativeDeps 'lib/cmake/Boost-1.85.0' }
    $build = Join-Path $PSScriptRoot 'native/build'
    $nativeTemp = Join-Path $build 'temp'
    [void][IO.Directory]::CreateDirectory($nativeTemp)
    $previousTemp = $env:TEMP
    $previousTmp = $env:TMP
    try {
        # MSVC 的中间文件使用项目下的路径，绕开中文用户目录的路径兼容问题。
        $env:TEMP = $nativeTemp
        $env:TMP = $nativeTemp
        if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
            & $VsShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        }
        $configureArgs = @(
            '-S', (Join-Path $PSScriptRoot 'native'), '-B', $build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TRY_COMPILE_CONFIGURATION=Release',
            "-DCMAKE_MAKE_PROGRAM=$Ninja", "-DCMAKE_PREFIX_PATH=$QtRoot;$NativeDeps",
            "-DOPENSSL_ROOT_DIR=$OpenSsl", "-DBoost_DIR=$BoostConfig"
        )
        & $CMake @configureArgs
        if ($LASTEXITCODE) { throw 'C++ 后台配置失败。' }
        $log = Join-Path $build 'build.log'
        & $CMake --build $build --target DesktopToolNative --parallel 6 *> $log
        if ($LASTEXITCODE) {
            Get-Content -LiteralPath $log -Tail 40
            throw "C++ 后台构建失败，请查看 $log"
        }
        [void][IO.Directory]::CreateDirectory($runtime)
        New-Item -Path "$build/.gdignore","$runtime/.gdignore" -ItemType File -Force | Out-Null
        Copy-Item -LiteralPath "$build/bin/DesktopToolNative.exe","$build/bin/DesktopToolIndexService.exe" -Destination $runtime -Force
        Copy-Item -LiteralPath "$build/bin/tools" -Destination $runtime -Recurse -Force
        $deploy = Join-Path $QtRoot 'bin/windeployqt.exe'
        & $deploy --release --no-translations --no-compiler-runtime --dir $runtime $backend
        if ($LASTEXITCODE) { throw '后台 Qt 运行库部署失败。' }
        $mediaArgs = @(
            '--release', '--no-translations', '--no-compiler-runtime', '--dir', "$runtime/tools",
            "$runtime/tools/DesktopToolImageBrowser.exe", "$runtime/tools/DesktopToolVideoPlayer.exe"
        )
        & $deploy @mediaArgs
        if ($LASTEXITCODE) { throw '独立媒体工具运行库部署失败。' }
        Get-ChildItem -LiteralPath "$OpenSsl/bin" -Filter 'lib*-3-x64.dll' |
            Copy-Item -Destination $runtime -Force
        Copy-Item -LiteralPath "$PSScriptRoot/native/third_party/licenses" -Destination $runtime -Recurse -Force
        Copy-Item -LiteralPath "$PSScriptRoot/native/third_party/minizip-ng-4.0.10/LICENSE" -Destination "$runtime/licenses/minizip-ng-LICENSE.txt" -Force
    } finally {
        $env:TEMP = $previousTemp
        $env:TMP = $previousTmp
    }
}

if ($BuildOnly) { return }

$godotArgs = @('--path', $PSScriptRoot)
if ($Editor) {
    $godotArgs += '--editor'
} else {
    $godotArgs += '--'
    $godotArgs += "--backend=$backend"
    if ($DataDirectory) {
        $godotArgs += '--data-dir=' + $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DataDirectory)
    }
}
& $GodotExe @godotArgs
