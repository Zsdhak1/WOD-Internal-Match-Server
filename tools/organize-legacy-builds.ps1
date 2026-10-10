[CmdletBinding(SupportsShouldProcess = $true)]
param()

# One-time, non-destructive migration for the pre-versioned build layout.
# Only explicitly listed legacy entries and loose root files are moved.
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root = (Resolve-Path (Join-Path $sourceRoot 'build')).Path
$running = Get-Process Scompetition, ScompetitionClient, RobotSimulator -ErrorAction SilentlyContinue
foreach ($process in $running) {
    if ($process.Path -and $process.Path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Close the running program before migration: $($process.Path)"
    }
}
$plan = [Collections.Generic.List[object]]::new()
function Add-Migration([string]$name, [string]$category, [string]$version, [string]$evidence) {
    $entry = Join-Path $root $name
    if (-not (Test-Path -LiteralPath $entry)) { return }
    $item = Get-Item -LiteralPath $entry
    $time = $item.LastWriteTimeUtc
    $timestampSource = 'entry-last-write-time'
    if ($item.PSIsContainer) {
        $apps = @()
        foreach ($location in @($item.FullName, (Join-Path $item.FullName 'bin'))) {
            if (Test-Path -LiteralPath $location) {
                $apps += Get-ChildItem -LiteralPath $location -File -Filter '*.exe' | Where-Object Name -in @('Scompetition.exe', 'ScompetitionClient.exe', 'RobotSimulator.exe')
            }
        }
        if ($apps.Count -gt 0) {
            $time = ($apps | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
            $timestampSource = 'latest-application-executable-last-write-time'
        }
    }
    if ($category -eq 'packages' -and $version -ne 'legacy-unknown') {
        $time = (Get-Item -LiteralPath $oldRelease).LastWriteTimeUtc
        $timestampSource = 'SHA256-matched-release-server-executable-last-write-time'
    }
    $timestamp = $time.ToString('yyyyMMdd-HHmmss')
    $bucket = if ($version -eq 'legacy-unknown') { $version } else { "v$version" }
    $leaf = if ($category -eq 'releases') { "${bucket}_${timestamp}_win64_Release" } elseif ($item.PSIsContainer -or $category -eq 'deprecated') { "${timestamp}_$name" } else { $name }
    $destination = if ($category -eq 'deprecated') {
        Join-Path $root "deprecated\$bucket\$leaf"
    } elseif ($category -eq 'packages') {
        Join-Path $root "packages\WOD-Internal-Match-Server_${bucket}_${timestamp}_win64_Release.zip"
    } else { Join-Path $root "$category\$leaf" }
    $plan.Add([pscustomobject]@{
        originalPath = $item.FullName; destinationPath = [IO.Path]::GetFullPath($destination)
        category = $category; version = $version; versionEvidence = $evidence
        timestampUtc = $timestamp; timestampSource = $timestampSource
        cacheRelocatable = $false
    })
}
foreach ($name in @('Desktop_Qt_6_10_2_MinGW_64_bit-Debug', 'msvc2022_64-VideoDebug', 'msvc2026_64-Debug', 'install-verify', 'install-verify-fixed', 'package-verify')) {
    $version = 'legacy-unknown'; $evidence = 'No embedded version metadata; not inferred from dates.'
    $cpack = Join-Path $root "$name\CPackConfig.cmake"
    if (Test-Path -LiteralPath $cpack) {
        $match = [regex]::Match((Get-Content -LiteralPath $cpack -Raw), 'set\(CPACK_PACKAGE_VERSION "([0-9.]+)"\)')
        if ($match.Success) { $version = $match.Groups[1].Value; $evidence = 'CPackConfig.cmake package version in the original cache.' }
    }
    Add-Migration $name 'deprecated' $version $evidence
}
# Verify that the old release actually matches the 0.1 Release cache before
# assigning its version. Otherwise preserve it under legacy-unknown.
$releaseVersion = 'legacy-unknown'; $releaseEvidence = 'No embedded version metadata.'
$oldRelease = Join-Path $root 'release-20261001\bin\Scompetition.exe'
$releaseCache = Join-Path $root 'msvc2022_64-VideoRelease\Scompetition.exe'
$cacheCpack = Join-Path $root 'msvc2022_64-VideoRelease\CPackConfig.cmake'
if ((Test-Path -LiteralPath $oldRelease) -and (Test-Path -LiteralPath $releaseCache) -and (Test-Path -LiteralPath $cacheCpack)) {
    if ((Get-FileHash -LiteralPath $oldRelease).Hash -eq (Get-FileHash -LiteralPath $releaseCache).Hash) {
        $match = [regex]::Match((Get-Content -LiteralPath $cacheCpack -Raw), 'set\(CPACK_PACKAGE_VERSION "([0-9.]+)"\)')
        if ($match.Success) { $releaseVersion = $match.Groups[1].Value; $releaseEvidence = 'Release server EXE SHA256 matches the retained cache; version from that cache CPackConfig.cmake.' }
    }
}
Add-Migration 'release-20261001' 'releases' $releaseVersion $releaseEvidence
# Keep the original ZIP unchanged; verify its server EXE matches the release.
$zipVersion = 'legacy-unknown'; $zipEvidence = 'No embedded version metadata.'
$oldZip = Join-Path $root 'WOD-Internal-Match-Server-20261001-win64.zip'
if ((Test-Path -LiteralPath $oldZip) -and (Test-Path -LiteralPath $oldRelease)) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($oldZip)
    try {
        $entries = @($archive.Entries | Where-Object { $_.FullName -match '(^|/)bin/Scompetition\.exe$' })
        if ($entries.Count -eq 1) {
            $stream = $entries[0].Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
                if ($hash -eq (Get-FileHash -LiteralPath $oldRelease).Hash) {
                    $zipVersion = $releaseVersion; $zipEvidence = 'ZIP server EXE SHA256 matches the retained legacy release; ' + $releaseEvidence
                }
            } finally { $stream.Dispose(); $sha.Dispose() }
        }
    } finally { $archive.Dispose() }
}
Add-Migration 'WOD-Internal-Match-Server-20261001-win64.zip' 'packages' $zipVersion $zipEvidence
Add-Migration 'msvc2022_64-VideoRelease.zip' 'deprecated' 'legacy-unknown' 'Earlier ZIP has no recorded build version; retained without relabeling.'
foreach ($name in @('layout-verification', 'responsive-validation')) { Add-Migration $name 'verification\legacy' 'legacy-unknown' 'Historical verification assets, not a software release.' }
foreach ($name in @('remote-esp32-judgement', 'remote-esp32-judgement-full')) { Add-Migration $name 'references' 'legacy-unknown' 'Upstream reference source, not this project build.' }
foreach ($item in Get-ChildItem -LiteralPath $root -File) {
    if ($plan.originalPath -contains $item.FullName) { continue }
    $category = switch ($item.Extension.ToLowerInvariant()) {
        '.png' { 'verification\legacy'; break }
        '.log' { 'logs\legacy'; break }
        '.reg' { 'backups'; break }
        '.json' { if ($item.Name -like 'upstream-*') { 'references' }; break }
        '.raw' { 'verification\legacy\video'; break }
        '.ts' { 'verification\legacy\video'; break }
        '.mjpg' { 'verification\legacy\video'; break }
        default { $null }
    }
    if ($category) { Add-Migration $item.Name $category 'legacy-unknown' 'Historical supporting asset, original filename preserved.' }
}
# Validate the complete plan before the first move. No delete/overwrite is used.
$destinations = @{}
foreach ($entry in $plan) {
    $from = (Resolve-Path -LiteralPath $entry.originalPath).Path
    $to = [IO.Path]::GetFullPath($entry.destinationPath)
    if (-not $from.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or -not $to.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Migration escaped build directory: $from -> $to" }
    if (Test-Path -LiteralPath $to) { throw "Destination already exists: $to" }
    if ($destinations.ContainsKey($to)) { throw "Duplicate destination: $to" }
    $destinations[$to] = $true
}
if ($WhatIfPreference) {
    $plan | Format-Table originalPath, destinationPath -Wrap
    return
}
if ($plan.Count -eq 0) { Write-Host 'Legacy layout already organized.'; return }
if (-not $PSCmdlet.ShouldProcess($root, "Move $($plan.Count) legacy entries without deletion; save migration manifest")) { return }
$manifest = Join-Path $root ('organization-' + [datetime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.json')
[ordered]@{ schemaVersion = 1; createdAtUtc = [datetime]::UtcNow.ToString('o'); entries = @($plan.ToArray()) } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding utf8
foreach ($entry in $plan) {
    # Resolve and check both targets again immediately before every move.
    $from = (Resolve-Path -LiteralPath $entry.originalPath).Path
    $to = [IO.Path]::GetFullPath($entry.destinationPath)
    if (-not $from.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or -not $to.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $to)) { throw "Unsafe migration target: $from -> $to" }
    New-Item -ItemType Directory -Path (Split-Path $to -Parent) -Force | Out-Null
    Move-Item -LiteralPath $from -Destination $to
    Write-Host "Moved: $from -> $to"
}
Write-Host "Manifest: $manifest"
Write-Host 'Retained development caches: msvc2022_64-Debug, msvc2022_64-VideoRelease. Archived caches are evidence only; reconfigure in a new path before building.'
