param(
    [Parameter(Mandatory = $true)][string]$Size,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$Executable,
    [string]$Screen = "secondary"
)

# Launches the server with the broadcast output forced to the requested
# window size, then captures the broadcast window from the screen.
$env:SCOMP_AUTO_BROADCAST = "1"
$env:SCOMP_BROADCAST_SIZE = $Size
$env:SCOMP_BROADCAST_SCREEN = $Screen
$env:SCOMP_MAIN_SCREEN = $Screen
$buildRoot = Join-Path $PSScriptRoot "..\build"
$latestExecutables = foreach ($configuration in @('Release', 'Debug')) {
    $index = Join-Path $buildRoot "latest-$configuration.json"
    if (Test-Path -LiteralPath $index) {
        $latest = Get-Content -LiteralPath $index -Raw | ConvertFrom-Json
        Join-Path $latest.releaseDirectory 'bin\Scompetition.exe'
    }
}
$candidates = @($latestExecutables) + @(
    (Join-Path $buildRoot "msvc2022_64-Debug\Scompetition.exe"),
    (Join-Path $buildRoot "msvc2022_64-VideoRelease\Scompetition.exe")
)
$exe = if ($Executable) {
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Executable)
} else {
    $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $exe -or -not (Test-Path -LiteralPath $exe)) {
    throw "Scompetition executable not found. Build it first or pass -Executable."
}
Write-Output "executable: $exe"
$proc = Start-Process -FilePath $exe -PassThru
try {
    Start-Sleep -Seconds 6

    Add-Type -AssemblyName System.Drawing
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
"@
    $parts = $Size -split 'x'
    $wantW = [int]$parts[0]
    $wantH = [int]$parts[1]
    $targetPid = [uint32]$proc.Id
    $found = [IntPtr]::Zero
    $cb = [Win32Cap+EnumProc]{
        param($h, $l)
        $pid2 = [uint32]0
        [Win32Cap]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
        if ($pid2 -eq $targetPid -and [Win32Cap]::IsWindowVisible($h)) {
            $r = New-Object Win32Cap+RECT
            [Win32Cap]::GetWindowRect($h, [ref]$r) | Out-Null
            if ([Math]::Abs(($r.Right - $r.Left) - $wantW) -le 40 -and
                [Math]::Abs(($r.Bottom - $r.Top) - $wantH) -le 40) {
                $script:found = $h
                return $false
            }
        }
        return $true
    }
    [Win32Cap]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    $hwnd = $found
    if ($hwnd -eq [IntPtr]::Zero) {
        foreach ($p in (Get-Process Scompetition -ErrorAction SilentlyContinue)) {
            Write-Output "process window: $($p.MainWindowTitle)"
        }
        throw "broadcast window not found"
    }
    # Keep the capture window on the selected display without moving it to
    # (0,0), which would put it on the operator's primary display. SWP_NOMOVE
    # and SWP_NOSIZE preserve the geometry chosen by the application.
    [Win32Cap]::SetWindowPos($hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0053) | Out-Null
    Start-Sleep -Milliseconds 800
    $rect = New-Object Win32Cap+RECT
    [Win32Cap]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    Write-Output "window rect: $($rect.Left),$($rect.Top) ${w}x${h}"
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "saved: $OutFile"
}
finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:SCOMP_AUTO_BROADCAST -ErrorAction SilentlyContinue
    Remove-Item Env:SCOMP_BROADCAST_SIZE -ErrorAction SilentlyContinue
    Remove-Item Env:SCOMP_BROADCAST_SCREEN -ErrorAction SilentlyContinue
    Remove-Item Env:SCOMP_MAIN_SCREEN -ErrorAction SilentlyContinue
}
