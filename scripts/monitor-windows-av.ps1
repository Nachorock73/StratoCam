[CmdletBinding()]
param(
    [ValidateRange(30, 3600)]
    [int]$DurationSeconds = 600,
    [ValidateRange(1, 30)]
    [int]$SampleSeconds = 5
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$process = Get-Process -Name 'StratoCam' -ErrorAction Stop | Select-Object -First 1

function Get-DeviceStatus {
    $client = [Net.Sockets.TcpClient]::new()
    $reader = $null
    $writer = $null
    try {
        $client.Connect('127.0.0.1', 27183)
        $stream = $client.GetStream()
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false), 1024, $true)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $false, 1024, $true)
        $writer.NewLine = "`n"
        $writer.WriteLine('{"version":1,"id":"av-monitor","command":"get_device_status","params":{}}')
        $writer.Flush()
        ($reader.ReadLine() | ConvertFrom-Json).deviceStatus
    } catch {
        $null
    } finally {
        if ($null -ne $reader) { $reader.Dispose() }
        if ($null -ne $writer) { $writer.Dispose() }
        $client.Dispose()
    }
}

$samples = [Collections.Generic.List[object]]::new()
$started = [DateTimeOffset]::Now
$initialDevice = Get-DeviceStatus
$previousCpu = $process.TotalProcessorTime.TotalSeconds
$previousTime = [Diagnostics.Stopwatch]::GetTimestamp()
$frequency = [Diagnostics.Stopwatch]::Frequency

while ((([DateTimeOffset]::Now - $started).TotalSeconds -lt $DurationSeconds) -and -not $process.HasExited) {
    Start-Sleep -Seconds $SampleSeconds
    $process.Refresh()
    $now = [Diagnostics.Stopwatch]::GetTimestamp()
    $cpu = $process.TotalProcessorTime.TotalSeconds
    $elapsed = ($now - $previousTime) / $frequency
    $singleCore = if ($elapsed -gt 0) { ($cpu - $previousCpu) / $elapsed * 100 } else { 0 }
    $gpu = $null
    $gpuVideoDecode = $null
    try {
        $counter = Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop
        $matching = @($counter.CounterSamples | Where-Object { $_.InstanceName -match "pid_$($process.Id)_" })
        if ($matching.Count) { $gpu = ($matching | Measure-Object CookedValue -Sum).Sum }
        $decode = @($matching | Where-Object { $_.InstanceName -match 'engtype_VideoDecode' })
        if ($decode.Count) { $gpuVideoDecode = ($decode | Measure-Object CookedValue -Sum).Sum }
    } catch { }
    $device = Get-DeviceStatus
    $samples.Add([pscustomobject]@{
        elapsedSeconds = [Math]::Round(([DateTimeOffset]::Now - $started).TotalSeconds, 1)
        cpuSingleCorePercent = [Math]::Round($singleCore, 2)
        cpuTotalMachinePercent = [Math]::Round($singleCore / [Environment]::ProcessorCount, 2)
        workingSetMB = [Math]::Round($process.WorkingSet64 / 1MB, 1)
        privateMemoryMB = [Math]::Round($process.PrivateMemorySize64 / 1MB, 1)
        gpuEnginePercent = if ($null -ne $gpu) { [Math]::Round($gpu, 2) } else { $null }
        gpuVideoDecodePercent = if ($null -ne $gpuVideoDecode) { [Math]::Round($gpuVideoDecode, 2) } else { $null }
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

$result = [pscustomobject]@{
    requestedDurationSeconds = $DurationSeconds
    started = $started
    finished = [DateTimeOffset]::Now
    initialDevice = $initialDevice
    finalDevice = Get-DeviceStatus
    pcSamples = $samples
}
$path = Join-Path $root ("captures\benchmark-wasapi-{0}.json" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
Write-Output $path
