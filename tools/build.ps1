[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$BuildDirectory,
    [string]$QtDirectory = 'G:\Dev\Qt\6.8.3\msvc2022_64',
    [string]$VisualStudioDirectory,
    [string]$WindowsSdkEnvironment,
    [ValidateRange(1, 128)][int]$Jobs = 4,
    [switch]$Test,
    [switch]$BuildOnly,
    [switch]$CleanFirst,
    [switch]$ConfigureOnly,
    [switch]$SplitPackages,
    [string]$PythonExecutable = 'python',
    [switch]$Plan
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cmakeText = Get-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Raw
$match = [regex]::Match($cmakeText, 'project\(Scompetition\s+VERSION\s+([0-9]+\.[0-9]+(?:\.[0-9]+)?)')
if (-not $match.Success) { throw 'Cannot read the project version from CMakeLists.txt.' }
$version = $match.Groups[1].Value
$timestamp = [datetime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$buildId = "v${version}_${timestamp}_win64_${Configuration}"
if (-not $VisualStudioDirectory) {
    $VisualStudioDirectory = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools',
        'D:\11AndroidLearnings\Env\Visual Studio 2026'
    ) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Common7\Tools\VsDevCmd.bat') } | Select-Object -First 1
}
if (-not $VisualStudioDirectory) { throw 'Pass -VisualStudioDirectory pointing to a Visual Studio installation.' }
$developer = Join-Path $VisualStudioDirectory 'Common7\Tools\VsDevCmd.bat'
$cmakeBin = Join-Path $VisualStudioDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninja = Join-Path $VisualStudioDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$cmake = Join-Path $cmakeBin 'cmake.exe'
foreach ($required in @($developer, $cmake, (Join-Path $ninja 'ninja.exe'), (Join-Path $QtDirectory 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required build dependency missing: $required" }
}
if (-not $BuildDirectory) {
    if ($VisualStudioDirectory -match '2022') {
        $BuildDirectory = if ($Configuration -eq 'Release') { 'build\msvc2022_64-VideoRelease' } elseif ($Configuration -eq 'Debug') { 'build\msvc2022_64-Debug' } else { "build\cache\msvc2022_64-$Configuration" }
    } else {
        $BuildDirectory = "build\cache\msvc2026_64-$Configuration"
    }
}
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $source $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$release = Join-Path $source "build\releases\$buildId"
$packageDirectory = Join-Path $source 'build\packages'
Write-Host "Version: $version | Build session (UTC): $timestamp"
Write-Host "Cache: $BuildDirectory"
Write-Host "Export: $release"
if ($SplitPackages) {
    Write-Host "Packages: $packageDirectory\Scompetition-{Server,Client}_$buildId.zip"
} else {
    Write-Host "Package: $packageDirectory\WOD-Internal-Match-Server_$buildId.zip"
}
if ($Plan) { return }
if (-not $SplitPackages -and -not $BuildOnly -and -not $ConfigureOnly -and (Test-Path -LiteralPath $release)) { throw "Export already exists; refusing to overwrite: $release" }

# Keep native diagnostics and PowerShell decoding on UTF-8. VSLANG is only a
# preference: an installation with just the Chinese clui.dll still uses Chinese.
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$env:VSLANG = '1033'
# Import the compiler environment into this process; quoted paths support spaces.
$sdkSwitch = if ($WindowsSdkEnvironment) { ' -winsdk=none' } else { '' }
$environmentCommand = 'call "' + $developer + '" -arch=x64 -host_arch=x64' + $sdkSwitch + ' >nul && set'
$environment = & $env:ComSpec /d /c $environmentCommand
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if ($WindowsSdkEnvironment) {
    if (-not (Test-Path -LiteralPath $WindowsSdkEnvironment -PathType Leaf)) { throw "SDK environment script not found: $WindowsSdkEnvironment" }
    . $WindowsSdkEnvironment
}
if ($Configuration -eq 'Debug') {
    $debugVersions = Join-Path $VisualStudioDirectory 'VC\Redist\MSVC'
    if (Test-Path -LiteralPath $debugVersions) {
        $debugCrt = Get-ChildItem -LiteralPath $debugVersions -Directory | Sort-Object Name -Descending | ForEach-Object {
            Get-ChildItem -LiteralPath (Join-Path $_.FullName 'debug_nonredist\x64') -Directory -ErrorAction SilentlyContinue
        } | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140d.dll') } | Select-Object -First 1
        if ($debugCrt) { $env:PATH = "$($debugCrt.FullName);$env:PATH" }
    }
}
$env:PATH = "$cmakeBin;$ninja;$env:PATH"
& $env:ComSpec /d /c 'chcp 65001 >nul'
# CMake's AUTO decoding can misread UTF-8 localized /showIncludes diagnostics.
# Measure the installed compiler's real prefix before generating Ninja rules.
$probeDirectory = Join-Path $BuildDirectory 'CMakeFiles\scomp-includes-probe'
New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
$probeSource = Join-Path $probeDirectory 'includes-probe.cpp'
$probeHeader = Join-Path $probeDirectory 'includes-probe.h'
$probeOutput = Join-Path $probeDirectory 'includes-probe.raw'
$probeObject = Join-Path $probeDirectory 'includes-probe.obj'
'#pragma once' | Set-Content -LiteralPath $probeHeader -Encoding ascii
'#include "includes-probe.h"' | Set-Content -LiteralPath $probeSource -Encoding ascii
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$probeCommand = 'chcp 65001 >nul && "' + $compiler + '" /nologo /c /showIncludes /Fo"' + $probeObject + '" "' + $probeSource + '" >"' + $probeOutput + '" 2>&1'
& $env:ComSpec /d /c $probeCommand
if ($LASTEXITCODE -ne 0) { throw "Compiler dependency probe failed: $probeOutput" }
$probeText = [Text.UTF8Encoding]::new($false, $true).GetString([IO.File]::ReadAllBytes($probeOutput))
$prefixMatch = [regex]::Match($probeText, '(?m)^([^:\r\n]+:[^:\r\n]+:[ \t]+)(?=[A-Za-z]:\\)')
if (-not $prefixMatch.Success) { throw "Cannot identify actual /showIncludes prefix: $probeOutput" }
$env:SCOMP_MSVC_INCLUDES_PREFIX = $prefixMatch.Groups[1].Value
Write-Host "Compiler dependency prefix: $($env:SCOMP_MSVC_INCLUDES_PREFIX)"
function Invoke-Checked([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed with exit code $LASTEXITCODE" }
}
$configureArguments = @('-S', $source, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_PREFIX_PATH=$QtDirectory", "-DSCOMP_BUILD_TIMESTAMP=$timestamp")
if ($WindowsSdkEnvironment) {
    # Forward slashes: CMake writes these verbatim into CMakeRCCompiler.cmake on a
    # fresh cache, where backslashes would be parsed as escapes (e.g. '\P').
    $configureArguments += "-DCMAKE_RC_COMPILER=$((Get-Command rc.exe -ErrorAction Stop).Source.Replace('\', '/'))"
    $configureArguments += "-DCMAKE_MT=$((Get-Command mt.exe -ErrorAction Stop).Source.Replace('\', '/'))"
}
Invoke-Checked $cmake $configureArguments
if ($ConfigureOnly) { return }
$buildArguments = @('--build', $BuildDirectory, '--config', $Configuration, '-j', "$Jobs")
if ($CleanFirst) { $buildArguments += '--clean-first' }
Invoke-Checked $cmake $buildArguments
if ($Configuration -eq 'Debug' -and $WindowsSdkEnvironment) {
    # windeployqt cannot locate Debug UCRT in the project-local NuGet SDK.
    # Keep this development-only DLL beside the binaries; never add it to Release packages.
    $debugUcrt = Join-Path $env:WindowsSdkVerBinPath 'x64\ucrt\ucrtbased.dll'
    if (-not (Test-Path -LiteralPath $debugUcrt -PathType Leaf)) { throw "Debug UCRT missing: $debugUcrt" }
    Invoke-Checked $cmake @('-E', 'copy_if_different', $debugUcrt, (Join-Path $BuildDirectory 'ucrtbased.dll'))
}
if ($Test) {
    # Qt's offscreen integration test needs the platform plugin when run from
    # a build tree; deployed release folders get plugins from windeployqt.
    $qtPlugins = Join-Path $QtDirectory 'plugins'
    if (Test-Path -LiteralPath (Join-Path $qtPlugins 'platforms')) {
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtPlugins 'platforms'
    }
    Invoke-Checked (Join-Path $cmakeBin 'ctest.exe') @('--test-dir', $BuildDirectory, '-C', $Configuration, '--output-on-failure')
}
if ($BuildOnly) { return }
if ($SplitPackages) {
    if ($Configuration -ne 'Release') { throw 'Standalone Windows packages require Release configuration.' }
    & (Join-Path $PSScriptRoot 'package-windows.ps1') -BuildDirectory $BuildDirectory -QtDirectory $QtDirectory -VisualStudioDirectory $VisualStudioDirectory -PythonExecutable $PythonExecutable
    return
}
Invoke-Checked $cmake @('--install', $BuildDirectory, '--config', $Configuration, '--prefix', $release)
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
Invoke-Checked (Join-Path $cmakeBin 'cpack.exe') @('--config', (Join-Path $BuildDirectory 'CPackConfig.cmake'), '-C', $Configuration, '-B', $packageDirectory)
$package = Join-Path $packageDirectory "WOD-Internal-Match-Server_$buildId.zip"
if (-not (Test-Path -LiteralPath $package)) { throw "Expected package not produced: $package" }
$index = [ordered]@{
    schemaVersion = 1; version = $version; buildId = $buildId
    buildTimestampUtc = $timestamp; completedAtUtc = [datetime]::UtcNow.ToString('o')
    configuration = $Configuration; cacheDirectory = $BuildDirectory
    releaseDirectory = $release; package = $package
    packageSha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    testsRun = [bool]$Test
}
$index | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $source "build\latest-$Configuration.json") -Encoding utf8
Write-Host "Completed: $buildId"
