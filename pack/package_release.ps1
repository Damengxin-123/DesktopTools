#Requires -Version 5.1
<#
.SYNOPSIS
从发布构建生成不含用户数据的便携 ZIP 压缩包。
.DESCRIPTION
每次通过 CMake 安装到全新的临时目录，仅打包声明的程序与运行资源。
不会读取或复制 out/bin、pack/app、用户数据目录或下载目录。
#>
[CmdletBinding()]
param(
    # 已完成编译的发布构建目录，默认使用项目的 x64-release 预设。
    [string]$BuildDirectory = '',
    # ZIP 和 SHA-256 校验文件的输出目录。
    [string]$OutputDirectory = '',
    # 可选 CMake 程序路径；未指定时优先复用构建缓存中的 CMake。
    [string]$CMakePath = $env:DESKTOPTOOL_CMAKE
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 从已配置构建的缓存中读取指定键，不执行缓存里的脚本文本。
function Get-CacheValue([string]$Name) {
    $pattern = '^' + [regex]::Escape($Name) + ':[^=]+=(.*)$'
    foreach ($line in $cacheLines) {
        if ($line -match $pattern) { return $Matches[1] }
    }
    return ''
}

# 只遍历普通目录检查链接；遇到链接立即停止，不进入其目标目录。
function Test-LinkFreeTree([string]$Directory) {
    foreach ($entry in Get-ChildItem -LiteralPath $Directory -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
        if ($entry.PSIsContainer -and -not (Test-LinkFreeTree $entry.FullName)) { return $false }
    }
    return $true
}
# 检查指定部署目录，拒绝用户数据、调试文件和指向目录外的链接。
function Assert-PackageTree([string]$Directory) {
    foreach ($entry in Get-ChildItem -LiteralPath $Directory -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "部署目录包含链接，已停止打包：$($entry.FullName)"
        }
        if ($entry.Name -match '^(data|logs?|downloads?|cache|GPUCache|QtWebEngine|\.git)$' -or
            $entry.Name -match '\.(pdb|ilk|obj|log|part|resume)$') {
            throw "部署规则包含用户数据或非发布文件，已停止打包：$($entry.FullName)"
        }
        if ($entry.PSIsContainer) { Assert-PackageTree $entry.FullName }
    }
}

$stagingDirectory = $null
$ownsStagingDirectory = $false
try {
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot '..\out\build\web-x64-release' }
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $PSScriptRoot '..\out\packages' }
    $buildPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
    $outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
    $cachePath = Join-Path $buildPath 'CMakeCache.txt'
    foreach ($required in @($cachePath, (Join-Path $buildPath 'cmake_install.cmake'), (Join-Path $buildPath 'bin\DesktopTool.exe'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "未找到完整发布构建：$required。请先执行 cmake --preset x64-release 和 cmake --build --preset x64-release。"
        }
    }
    $cacheLines = Get-Content -LiteralPath $cachePath
    if ((Get-CacheValue 'CMAKE_PROJECT_NAME') -ne 'DesktopTool' -or (Get-CacheValue 'CMAKE_BUILD_TYPE') -ne 'Release') {
        throw '仅支持 DesktopTool 的 Release 构建，请检查 -BuildDirectory。'
    }
    $version = Get-CacheValue 'CMAKE_PROJECT_VERSION'
    if ($version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw '构建缓存中的版本号无效，请重新配置发布构建。' }
    if ([string]::IsNullOrWhiteSpace($CMakePath)) { $CMakePath = Get-CacheValue 'CMAKE_COMMAND' }
    if ([string]::IsNullOrWhiteSpace($CMakePath)) { $CMakePath = 'cmake.exe' }
    $cmakeCommand = Get-Command -Name $CMakePath -CommandType Application -ErrorAction Stop

    # 每次使用独立空目录，避免旧发布目录中遗留的个人文件进入 ZIP。
    [IO.Directory]::CreateDirectory($outputPath) | Out-Null
    $stagingName = '.desktoptool-package-' + [guid]::NewGuid().ToString('N')
    $stagingDirectory = Join-Path $outputPath $stagingName
    if (Test-Path -LiteralPath $stagingDirectory) { throw '临时目录已存在，请重试。' }
    [IO.Directory]::CreateDirectory($stagingDirectory) | Out-Null
    $ownsStagingDirectory = $true
    $packageName = "DesktopTool-$version-windows-x64"
    $payloadDirectory = Join-Path $stagingDirectory $packageName
    $installLog = Join-Path $stagingDirectory 'install.log'
    Write-Host '正在部署程序、Qt 运行库、WebEngine 资源和许可证…'
    # Windows PowerShell 5.1 会将原生 stderr 包装为错误；安装结果以原生退出码为准。
    $ErrorActionPreference = 'Continue'
    & $cmakeCommand.Source --install $buildPath --config Release --prefix $payloadDirectory *> $installLog
    $installExitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($installExitCode -ne 0) {
        Get-Content -LiteralPath $installLog -Tail 40 | Write-Host
        throw "CMake 部署失败，错误码：$installExitCode。"
    }
    Assert-PackageTree $payloadDirectory
    # HTML、CSS、JavaScript 和应用图标已编译进 exe；以下是必须随附的外部资源。
    $requiredFiles = @('tools\DesktopToolImageBrowser.exe', 'tools\DesktopToolVideoPlayer.exe',
        'tools\Qt6Core.dll', 'tools\Qt6Widgets.dll', 'tools\Qt6Multimedia.dll', 'tools\plugins\platforms\qwindows.dll', 'tools\qt.conf',
        'DesktopTool.exe', 'DesktopToolLevelEditor.exe', 'DesktopToolIndexService.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
        'Qt6Network.dll', 'Qt6WebChannel.dll', 'Qt6WebEngineCore.dll', 'Qt6WebEngineWidgets.dll',
        'QtWebEngineProcess.exe', 'plugins\platforms\qwindows.dll', 'resources\icudtl.dat',
        'resources\qtwebengine_resources.pak', 'resources\qtwebengine_resources_100p.pak',
        'resources\qtwebengine_resources_200p.pak', 'resources\v8_context_snapshot.bin', 'qt.conf', 'licenses\LIBTORRENT.txt', 'licenses\OPENSSL.txt')
    foreach ($relativePath in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $payloadDirectory $relativePath) -PathType Leaf)) {
            throw "发布资源缺失：$relativePath。请检查 Qt 部署与运行库配置。"
        }
    }
    $localeDirectory = Join-Path $payloadDirectory 'translations\qtwebengine_locales'
    if (-not (Test-Path -LiteralPath $localeDirectory -PathType Container) -or
        @(Get-ChildItem -LiteralPath $localeDirectory -Filter '*.pak' -File).Count -eq 0) {
        throw '缺少 WebEngine 语言资源，已停止打包。'
    }
    # 注入安装器：卸载脚本随程序目录部署，安装脚本放压缩包根目录。
    # 模板中的占位符在打包时替换为实际版本号与程序目录名。
    $installerTemplate = Join-Path $PSScriptRoot 'installer-template.bat'
    $uninstallerTemplate = Join-Path $PSScriptRoot 'uninstaller-template.bat'
    foreach ($template in @($installerTemplate, $uninstallerTemplate)) {
        if (-not (Test-Path -LiteralPath $template -PathType Leaf)) {
            throw "缺少安装器模板：$template"
        }
    }
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    # cmd 按 ANSI 代码页逐行解析批处理；UTF-8 中文会被误切分出杂散命令。
    # 模板以 UTF-8 入库，打包时统一转成 GBK（代码页 936）+ CRLF 再生成脚本。
    $gbk = [Text.Encoding]::GetEncoding(936)
    function Convert-BatchScript([string]$SourceText) {
        $prepared = $SourceText.Replace("chcp 65001", "chcp 936").Replace("`r`n", "`n")
        return ($prepared -replace "`n", "`r`n")
    }
    $uninstallerSource = [IO.File]::ReadAllText($uninstallerTemplate, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $payloadDirectory 'uninstall.bat'),
        (Convert-BatchScript $uninstallerSource), $gbk)
    $installerSource = [IO.File]::ReadAllText($installerTemplate, $utf8NoBom)
    $installerScript = Convert-BatchScript($installerSource.Replace('@VERSION@', $version).Replace('@PAYLOAD_DIR@', $packageName))
    $installScriptPath = Join-Path $stagingDirectory 'install.bat'
    [IO.File]::WriteAllText($installScriptPath, $installerScript, $gbk)
    $fileCount = @(Get-ChildItem -LiteralPath $payloadDirectory -File -Recurse -Force).Count
    $zipName = $packageName + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.zip'
    $zipPath = Join-Path $outputPath $zipName
    $temporaryZip = Join-Path $stagingDirectory 'package.zip'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Write-Host "正在压缩 $fileCount 个程序与资源文件…"
    # 使用 .NET ZIP API，保留所有部署文件，支持 ZIP64，不依赖第三方压缩程序。
    # 压缩包根目录 = 程序目录 + install.bat：解压后运行 install.bat 即为安装，
    # 直接使用程序目录则为便携模式。
    $zipSourceDirectory = Join-Path $stagingDirectory 'zipsrc'
    [IO.Directory]::CreateDirectory($zipSourceDirectory) | Out-Null
    Move-Item -LiteralPath $payloadDirectory -Destination (Join-Path $zipSourceDirectory $packageName)
    Copy-Item -LiteralPath $installScriptPath -Destination (Join-Path $zipSourceDirectory 'install.bat')
    [IO.Compression.ZipFile]::CreateFromDirectory($zipSourceDirectory, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    # 直接使用系统 .NET 计算校验值，兼容未加载 Get-FileHash 模块的 PowerShell 环境。
    $hashAlgorithm = [Security.Cryptography.SHA256]::Create()
    $zipStream = [IO.File]::OpenRead($temporaryZip)
    try {
        $checksum = [BitConverter]::ToString($hashAlgorithm.ComputeHash($zipStream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $zipStream.Dispose()
        $hashAlgorithm.Dispose()
    }
    # File.Move 在目标已存在时失败，绝不覆盖已有压缩包。
    [IO.File]::Move($temporaryZip, $zipPath)
    [IO.File]::WriteAllText(($zipPath + '.sha256'), "$checksum *$zipName" + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
    Write-Host "打包完成：$zipPath"
    Write-Host ('压缩包大小：{0:N1} MB；未包含本地用户数据。' -f ((Get-Item -LiteralPath $zipPath).Length / 1MB))
    Write-Host '安装方式：解压后运行 install.bat，按提示输入安装位置（会在其中创建 DesktopTool 子目录）。'
    Write-Host "SHA-256：$checksum"
} catch {
    [Console]::Error.WriteLine('打包失败：' + $_.Exception.Message)
    exit 1
} finally {
    if ($ownsStagingDirectory -and (Test-Path -LiteralPath $stagingDirectory)) {
        # 递归清理前校验绝对路径和所有权，只移除本次创建的临时目录。
        $resolvedStaging = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $stagingDirectory).Path)
        $expectedStaging = [IO.Path]::GetFullPath((Join-Path $outputPath $stagingName))
        $stagingEntry = Get-Item -LiteralPath $resolvedStaging -Force
        if ($resolvedStaging -eq $expectedStaging -and
            [IO.Path]::GetDirectoryName($resolvedStaging).TrimEnd('\') -eq $outputPath.TrimEnd('\') -and
            ($stagingEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and (Test-LinkFreeTree $resolvedStaging)) {
            Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
        } else {
            Write-Warning "临时目录边界校验失败，已保留：$stagingDirectory"
        }
    }
}
