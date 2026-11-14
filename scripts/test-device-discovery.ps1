[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$realAdb = Join-Path $root '.tools\android-sdk\platform-tools\adb.exe'
$directory = Join-Path $root ('captures\device-discovery-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$platformTools = Join-Path $directory 'platform-tools'
[IO.Directory]::CreateDirectory($platformTools) | Out-Null
Copy-Item -Path (Join-Path $root 'windows\build\bin\*.dll') -Destination $directory
Copy-Item -LiteralPath (Join-Path $root 'windows\build\bin\StratoCam.exe') -Destination $directory
& (Join-Path $root '.tools\winlibs\mingw64\bin\g++.exe') -std=c++17 -municode -static `
    (Join-Path $root 'windows\tests\DiscoveryAdbFixture.cpp') -o (Join-Path $platformTools 'adb.exe')
if ($LASTEXITCODE -ne 0) { throw 'Could not compile the diagnostic ADB gate.' }
[IO.File]::WriteAllText((Join-Path $platformTools 'real-adb.txt'), $realAdb)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DiscoveryTestWindows {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr value);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr w, IntPtr l);
    public static void Close(uint targetPid) {
        EnumWindows((hwnd, value) => { uint pid; GetWindowThreadProcessId(hwnd, out pid);
            if (pid == targetPid) PostMessage(hwnd, 0x0010, IntPtr.Zero, IntPtr.Zero); return true; }, IntPtr.Zero);
    }
}
'@

$log = Join-Path $env:LOCALAPPDATA 'StratoCam\Diagnostics\windows-pipeline-diagnostics.log'
function Wait-Log([string]$Pattern, [int]$Seconds = 45) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if ($process.HasExited) { throw "Diagnostic application exited: $($process.ExitCode)" }
        if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern $Pattern -Quiet)) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for diagnostic event: $Pattern"
}

$process = Start-Process -FilePath (Join-Path $directory 'StratoCam.exe') -WindowStyle Hidden -PassThru
try {
    Wait-Log 'Discovery retry scheduled'
    # Keep the fake device absent for two attempts, then expose the actual USB phone.
    Start-Sleep -Seconds 4
    $absentLog = Get-Content -LiteralPath $log -Raw
    if ($absentLog.Contains('Decoder ready')) { throw 'Video started while the test device was absent.' }
    [IO.File]::WriteAllText((Join-Path $platformTools 'phone-arrived'), 'ready')
    Wait-Log 'Decoder ready; starting video transport'
    Wait-Log 'STREAMING first H.264 access unit received'
    Wait-Log 'decoded-frame slot submitted=[1-9]'
    Copy-Item -LiteralPath $log -Destination (Join-Path $directory 'pipeline.log')
    $result = [ordered]@{
        scenario = 'empty discovery -> automatic retry -> real USB device -> decoder'
        passed = $true
        retriesWhileAbsent = @([regex]::Matches($absentLog, 'Discovery retry scheduled')).Count
        outputDirectory = $directory
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding UTF8
    [pscustomobject]$result
} finally {
    [DiscoveryTestWindows]::Close([uint32]$process.Id)
    if (-not $process.WaitForExit(20000)) { Stop-Process -Id $process.Id -Force; throw 'Diagnostic process did not close promptly.' }
}
