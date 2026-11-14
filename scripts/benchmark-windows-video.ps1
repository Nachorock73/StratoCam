[CmdletBinding()]
param(
    [ValidateSet('1080p30', '4k30')]
    [string]$Mode,
    [ValidateRange(30, 1800)]
    [int]$DurationSeconds,
    [ValidateRange(1, 30)]
    [int]$SampleSeconds = 5,
    [switch]$DirectRecording,
    [switch]$ExerciseRecordingLifecycle,
    [string]$ApplicationPath,
    [string]$ApplicationWorkingDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = if ($ApplicationPath) { [IO.Path]::GetFullPath($ApplicationPath) } else {
    Join-Path $root 'windows\build\bin\StratoCam.exe'
}
$appWorkingDirectory = if ($ApplicationWorkingDirectory) {
    [IO.Path]::GetFullPath($ApplicationWorkingDirectory)
} else { Split-Path -Parent $app }
$adb = Join-Path $root '.tools\android-sdk\platform-tools\adb.exe'
if (-not (Test-Path -LiteralPath $app)) { throw 'Build the Windows application first.' }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AndroidCamBenchmarkNative {
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr parent, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
}
'@

function Get-DeviceStatus {
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.Connect('127.0.0.1', 27183)
        $stream = $client.GetStream()
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false), 1024, $true)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $false, 1024, $true)
        $writer.NewLine = "`n"
        $writer.WriteLine('{"version":1,"id":"benchmark","command":"get_device_status","params":{}}')
        $writer.Flush()
        ($reader.ReadLine() | ConvertFrom-Json).deviceStatus
    } catch { $null } finally {
        if ($null -ne $reader) { $reader.Dispose() }
        if ($null -ne $writer) { $writer.Dispose() }
        $client.Dispose()
    }
}

& $adb forward 'tcp:27183' 'tcp:27183' | Out-Null
$process = Start-Process -FilePath $app -WorkingDirectory $appWorkingDirectory -PassThru
try {
    for ($attempt = 0; $attempt -lt 50 -and $process.MainWindowHandle -eq 0; $attempt++) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
    }
    if ($process.MainWindowHandle -eq 0) { throw 'StratoCam window did not open.' }
    Start-Sleep -Seconds 3
    if ($Mode -eq '4k30') {
        $combo = [AndroidCamBenchmarkNative]::GetDlgItem($process.MainWindowHandle, 121)
        if ($combo -eq [IntPtr]::Zero) { throw 'Video mode selector was not found.' }
        [AndroidCamBenchmarkNative]::SendMessage($combo, 0x014E, [IntPtr]1, [IntPtr]0) | Out-Null
        $command = [IntPtr]((1 -shl 16) -bor 121)
        [AndroidCamBenchmarkNative]::SendMessage($process.MainWindowHandle, 0x0111, $command, $combo) | Out-Null
        Start-Sleep -Seconds 5
    }
    $recordButton = [IntPtr]::Zero
    if ($DirectRecording) {
        $recordButton = [AndroidCamBenchmarkNative]::GetDlgItem($process.MainWindowHandle, 122)
        if ($recordButton -eq [IntPtr]::Zero) { throw 'Direct-recording button was not found.' }
        [AndroidCamBenchmarkNative]::SendMessage($recordButton, 0x00F5, [IntPtr]0, [IntPtr]0) | Out-Null
        Start-Sleep -Seconds 2
    }
    $initialDevice = Get-DeviceStatus
    $samples = [Collections.Generic.List[object]]::new()
    $previousCpu = $process.TotalProcessorTime.TotalSeconds
    $previousTime = [Diagnostics.Stopwatch]::GetTimestamp()
    $frequency = [Diagnostics.Stopwatch]::Frequency
    $started = [DateTimeOffset]::Now
    $events = [Collections.Generic.List[string]]::new()
    $zoomChanged = $focusChanged = $exposureChanged = $minimized = $restored = $controlsRestored = $false
    while ((([DateTimeOffset]::Now - $started).TotalSeconds -lt $DurationSeconds) -and -not $process.HasExited) {
        Start-Sleep -Seconds $SampleSeconds
        $process.Refresh()
        $now = [Diagnostics.Stopwatch]::GetTimestamp()
        $cpu = $process.TotalProcessorTime.TotalSeconds
        $elapsed = ($now - $previousTime) / $frequency
        $singleCore = if ($elapsed -gt 0) { ($cpu - $previousCpu) / $elapsed * 100 } else { 0 }
        $gpu = $null
        $gpuVideoDecode = $null
        $gpuVideoDecodeSystem = $null
        try {
            $counter = Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop
            $matching = @($counter.CounterSamples | Where-Object { $_.InstanceName -match "pid_$($process.Id)_" })
            if ($matching.Count) { $gpu = ($matching | Measure-Object CookedValue -Sum).Sum }
            $decode = @($matching | Where-Object { $_.InstanceName -match 'engtype_VideoDecode' })
            if ($decode.Count) { $gpuVideoDecode = ($decode | Measure-Object CookedValue -Sum).Sum }
            $systemDecode = @($counter.CounterSamples | Where-Object { $_.InstanceName -match 'engtype_VideoDecode' })
            if ($systemDecode.Count) { $gpuVideoDecodeSystem = ($systemDecode | Measure-Object CookedValue -Sum).Sum }
        } catch { }
        $device = Get-DeviceStatus
        $progress = ([DateTimeOffset]::Now - $started).TotalSeconds / $DurationSeconds
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $zoomChanged -and $progress -ge 0.15) {
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-zoom -ZoomRatio 1.5 | Out-Null
            $events.Add('zoom 1.5x')
            $zoomChanged = $true
        }
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $focusChanged -and $progress -ge 0.25) {
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-focus -Mode Manual -FocusDistanceDiopters 2.0 | Out-Null
            $events.Add('focus manual 2.0D')
            $focusChanged = $true
        }
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $exposureChanged -and $progress -ge 0.35) {
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-exposure -Mode Manual -Iso 200 -ExposureTimeNs 16666667 | Out-Null
            $events.Add('exposure manual ISO 200 1/60s')
            $exposureChanged = $true
        }
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $minimized -and $progress -ge 0.45) {
            [AndroidCamBenchmarkNative]::ShowWindowAsync($process.MainWindowHandle, 6) | Out-Null
            $events.Add('window minimized')
            $minimized = $true
        }
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $restored -and $progress -ge 0.55) {
            [AndroidCamBenchmarkNative]::ShowWindowAsync($process.MainWindowHandle, 9) | Out-Null
            $events.Add('window restored')
            $restored = $true
        }
        if ($DirectRecording -and $ExerciseRecordingLifecycle -and -not $controlsRestored -and $progress -ge 0.70) {
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-focus -Mode Auto | Out-Null
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-exposure -Mode Auto | Out-Null
            & (Join-Path $PSScriptRoot 'android-cam-control.ps1') set-zoom -ZoomRatio 1.0 | Out-Null
            $events.Add('focus/exposure auto; zoom 1x')
            $controlsRestored = $true
        }
        $samples.Add([pscustomobject]@{
            elapsedSeconds = [Math]::Round(([DateTimeOffset]::Now - $started).TotalSeconds, 1)
            cpuSingleCorePercent = [Math]::Round($singleCore, 2)
            cpuTotalMachinePercent = [Math]::Round($singleCore / [Environment]::ProcessorCount, 2)
            workingSetMB = [Math]::Round($process.WorkingSet64 / 1MB, 1)
            privateMemoryMB = [Math]::Round($process.PrivateMemorySize64 / 1MB, 1)
            gpuEnginePercent = if ($null -ne $gpu) { [Math]::Round($gpu, 2) } else { $null }
            gpuVideoDecodePercent = if ($null -ne $gpuVideoDecode) { [Math]::Round($gpuVideoDecode, 2) } else { $null }
            gpuVideoDecodeSystemPercent = if ($null -ne $gpuVideoDecodeSystem) { [Math]::Round($gpuVideoDecodeSystem, 2) } else { $null }
            batteryPercent = $device.batteryPercent
            batteryState = $device.batteryState
            powerSource = $device.powerSource
            batteryTemperatureC = $device.batteryTemperatureC
            currentNowMilliamps = $device.currentNowMilliamps
            currentAverageMilliamps = $device.currentAverageMilliamps
            voltageVolts = $device.voltageVolts
            approximatePowerWatts = $device.approximatePowerWatts
            thermalStatus = $device.thermalStatus
        })
        $previousCpu = $cpu
        $previousTime = $now
    }
    $finalDevice = Get-DeviceStatus
} finally {
    if ($DirectRecording -and $null -ne $recordButton -and $recordButton -ne [IntPtr]::Zero -and -not $process.HasExited) {
        [AndroidCamBenchmarkNative]::SendMessage($recordButton, 0x00F5, [IntPtr]0, [IntPtr]0) | Out-Null
        Start-Sleep -Seconds 2
    }
    if (-not $process.HasExited) {
        [AndroidCamBenchmarkNative]::PostMessage($process.MainWindowHandle, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
        if (-not $process.WaitForExit(10000)) { $process.CloseMainWindow() | Out-Null }
    }
}

$result = [pscustomobject]@{
    mode = $Mode
    applicationPath = $app
    requestedDurationSeconds = $DurationSeconds
    started = $started
    finished = [DateTimeOffset]::Now
    initialDevice = $initialDevice
    finalDevice = $finalDevice
    pcSamples = $samples
    directRecording = $DirectRecording.IsPresent
    lifecycleEvents = $events
}
$path = Join-Path $root ("captures\benchmark-{0}-{1}.json" -f $Mode, (Get-Date -Format 'yyyyMMdd-HHmmss'))
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
Write-Host $path
$result
