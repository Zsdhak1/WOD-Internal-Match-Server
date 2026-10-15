[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$QtDirectory,
    [Parameter(Mandatory)][string]$VisualStudioDirectory,
    [string]$PythonExecutable = 'python'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDirectory = (Resolve-Path $BuildDirectory).Path
$info = Get-Content -LiteralPath (Join-Path $BuildDirectory 'build-info.json') -Raw | ConvertFrom-Json
if ($info.platform -ne 'win64' -or $info.configuration -ne 'Release') { throw 'Expected a Windows x64 Release build.' }
$buildId = $info.buildId
$export = Join-Path $source "build\releases\$buildId-split"
$packages = Join-Path $source 'build\packages'
if (Test-Path -LiteralPath $export) { throw "Refusing to overwrite export: $export" }
New-Item -ItemType Directory -Path $export, $packages -Force | Out-Null
$deploy = Join-Path $QtDirectory 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $deploy)) { throw 'windeployqt is required for self-contained Qt packages.' }
function Run([string]$program, [string[]]$arguments) {
    # Windows PowerShell wraps ordinary native stderr (e.g. PyInstaller INFO)
    # as NativeCommandError when redirected; use the actual exit code instead.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $program @arguments; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $savedPreference }
    if ($code -ne 0) { throw "$program failed with exit code $code" }
}
# Explicit app-local CRT avoids requiring VC++ redist installation on the target.
$redist = Join-Path $VisualStudioDirectory 'VC\Redist\MSVC'
$crt = Get-ChildItem -LiteralPath $redist -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
    # VS 2022 ships Microsoft.VC143.CRT, VS 2026 ships Microsoft.VC145.CRT.
    ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -ExpandProperty FullName } |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'msvcp140.dll') } | Select-Object -First 1
if (-not $crt) { throw 'MSVC x64 app-local runtime was not found.' }
function Copy-Crt([string]$destination) {
    Copy-Item -Path (Join-Path $crt '*.dll') -Destination $destination
}
function Write-Utf8([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}
$names = [ordered]@{ Server = 'Scompetition.exe'; Client = 'ScompetitionClient.exe' }
$archives = @()
foreach ($component in $names.Keys) {
    $folder = Join-Path $export "Scompetition-${component}_$buildId"
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $exe = $names[$component]
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $exe) -Destination $folder
    Run $deploy @('--release', '--compiler-runtime', '--no-translations', '--dir', $folder, (Join-Path $folder $exe))
    Copy-Crt $folder
    Copy-Item -LiteralPath (Join-Path $BuildDirectory 'build-info.json') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $source 'broadcast_layout.ini') -Destination $folder
    New-Item -ItemType Directory -Path (Join-Path $folder 'tools'), (Join-Path $folder 'Assets'), (Join-Path $folder 'docs') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'tools\ffmpeg') -Destination (Join-Path $folder 'tools') -Recurse
    Copy-Item -LiteralPath (Join-Path $source 'Assets\gamefinishvideo') -Destination (Join-Path $folder 'Assets') -Recurse
    Copy-Item -LiteralPath (Join-Path $source 'README.md'), (Join-Path $source 'CHANGELOG.md') -Destination $folder
    Copy-Item -Path (Join-Path $source 'docs\*.md') -Destination (Join-Path $folder 'docs')
    Write-Utf8 (Join-Path $folder 'qt.conf') "[Paths]`nPrefix=.`nPlugins=.`n"
    Write-Utf8 (Join-Path $folder 'Start.cmd') "@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" `"%~dp0$exe`"`r`n"
    Write-Utf8 (Join-Path $folder 'START-HERE.txt') "Windows 10/11 x64 portable $component. Extract the complete ZIP, then run Start.cmd or $exe.`r`nQt/plugins, MSVC CRT, static FFmpeg and settlement assets are bundled. No Qt/Python install required.`r`nCurrent kill-score/SD rules are not the official BO3 model. See README and docs.`r`n"
}
# results-web is deprecated: only the Server and Client desktop packages are built.
Run $PythonExecutable @((Join-Path $PSScriptRoot 'verify-windows-packages.py'), '--directory', $export)
foreach ($folder in Get-ChildItem -LiteralPath $export -Directory) {
    $manifest = [ordered]@{ component = $folder.Name; platform = 'win64'; configuration = 'Release'; build = $info; files = @() }
    foreach ($file in Get-ChildItem -LiteralPath $folder.FullName -File -Recurse) {
        $manifest.files += [ordered]@{ path = $file.FullName.Substring($folder.FullName.Length + 1).Replace('\', '/'); bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
    Write-Utf8 (Join-Path $folder.FullName 'PACKAGE_MANIFEST.json') ($manifest | ConvertTo-Json -Depth 8)
    $zip = Join-Path $packages "$($folder.Name).zip"
    if (Test-Path -LiteralPath $zip) { throw "Refusing to overwrite ZIP: $zip" }
    # System tar uses libarchive ZIP format, handles large payloads and preserves
    # a top-level component directory (unlike Compress-Archive's size limits).
    Run 'tar.exe' @('-a', '-c', '-f', $zip, '-C', $export, $folder.Name)
    $archives += [ordered]@{ component = $folder.Name; path = $zip; sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash }
}
$archiveArgs = @((Join-Path $PSScriptRoot 'verify-windows-packages.py'), '--directory', $export, '--archives') + @($archives | ForEach-Object { $_.path })
Run $PythonExecutable $archiveArgs
$checksum = Join-Path $packages "SHA256SUMS_$buildId.txt"
Write-Utf8 $checksum (($archives | ForEach-Object { "$($_.sha256.ToLower())  $([IO.Path]::GetFileName($_.path))" }) -join "`n")
$releaseIndex = [ordered]@{ schemaVersion = 1; buildId = $buildId; exportDirectory = $export; packages = $archives; checksums = $checksum; completedAtUtc = [datetime]::UtcNow.ToString('o') }
Write-Utf8 (Join-Path $source 'build\latest-split.json') ($releaseIndex | ConvertTo-Json -Depth 6)
Write-Host "Completed Server and Client standalone Windows x64 packages: $buildId"
