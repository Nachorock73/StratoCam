[CmdletBinding()]
param(
    [ValidateSet('Auto', 'FFplay', 'VLC')]
    [string]$Decoder = 'VLC',

    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 0,

    [ValidateRange(0, 2000)]
    [int]$VlcCachingMs = 0,

    [string]$DiagnosticCopyPath,

    [string]$LatencyCsvPath
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $projectRoot '.tools\android-sdk\platform-tools\adb.exe'
if (-not (Test-Path -LiteralPath $adb)) {
    throw "ADB not found at $adb. Build the project dependencies or install Android platform-tools."
}

$projectAdbKey = Join-Path $projectRoot '.tools\adb-keys\adbkey'
if (Test-Path -LiteralPath $projectAdbKey) { $env:ADB_VENDOR_KEYS = $projectAdbKey }

function Find-Decoder {
    param([string]$RequestedDecoder)

    $localFfplay = Join-Path $projectRoot '.tools\ffmpeg\bin\ffplay.exe'
    $ffplayCommand = Get-Command 'ffplay.exe' -ErrorAction SilentlyContinue
    $ffplay = if (Test-Path -LiteralPath $localFfplay) {
        $localFfplay
    } elseif ($null -ne $ffplayCommand) {
        $ffplayCommand.Source
    } else {
        $null
    }

    $vlcCandidates = @(
        (Join-Path $projectRoot '.tools\vlc\vlc.exe'),
        'C:\Program Files\VideoLAN\VLC\vlc.exe',
        'C:\Program Files (x86)\VideoLAN\VLC\vlc.exe'
    )
    $vlc = $vlcCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($null -eq $vlc) {
        $vlcCommand = Get-Command 'vlc.exe' -ErrorAction SilentlyContinue
        if ($null -ne $vlcCommand) { $vlc = $vlcCommand.Source }
    }

    if ($RequestedDecoder -eq 'FFplay') {
        if ($null -eq $ffplay) { throw 'FFplay was requested but ffplay.exe was not found.' }
        return [pscustomobject]@{ name = 'FFplay'; path = $ffplay }
    }
    if ($RequestedDecoder -eq 'VLC') {
        if ($null -eq $vlc) { throw 'VLC was requested but vlc.exe was not found.' }
        return [pscustomobject]@{ name = 'VLC'; path = $vlc }
    }
    if ($null -ne $ffplay) { return [pscustomobject]@{ name = 'FFplay'; path = $ffplay } }
    if ($null -ne $vlc) { return [pscustomobject]@{ name = 'VLC'; path = $vlc } }
    throw 'No supported decoder was found. Install FFmpeg/ffplay or VLC from a trusted source.'
}

function Read-Exactly {
    param(
        [Parameter(Mandatory)] [System.IO.BinaryReader]$Reader,
        [Parameter(Mandatory)] [int]$Count
    )
    try {
        $bytes = $Reader.ReadBytes($Count)
    } catch {
        throw "The Android video stream stopped or timed out while waiting for data: $($_.Exception.Message)"
    }
    if ($bytes.Length -ne $Count) {
        throw "The Android video socket closed after $($bytes.Length) of $Count expected bytes."
    }
    $bytes
}

function Get-MonotonicNanoseconds {
    ([double][System.Diagnostics.Stopwatch]::GetTimestamp() * 1000000000.0) /
        [System.Diagnostics.Stopwatch]::Frequency
}

function Invoke-ClockSynchronization {
    param([Parameter(Mandatory)] [System.Net.Sockets.NetworkStream]$Stream)

    $syncMagic = 0x31534341
    $syncVersion = 1
    $syncSamples = 9
    $writer = [System.IO.BinaryWriter]::new($Stream, [System.Text.Encoding]::UTF8, $true)
    $reader = [System.IO.BinaryReader]::new($Stream, [System.Text.Encoding]::UTF8, $true)
    $results = [System.Collections.Generic.List[object]]::new()
    try {
        for ($sequence = 0; $sequence -lt $syncSamples; $sequence += 1) {
            $windowsSendNs = Get-MonotonicNanoseconds
            $writer.Write([int]$syncMagic)
            $writer.Write([int]$syncVersion)
            $writer.Write([long]$sequence)
            $writer.Flush()
            $responseBytes = Read-Exactly -Reader $reader -Count 32
            $windowsReceiveNs = Get-MonotonicNanoseconds
            $responseStream = [System.IO.MemoryStream]::new($responseBytes, $false)
            $response = [System.IO.BinaryReader]::new($responseStream)
            try {
                $magic = $response.ReadInt32()
                $version = $response.ReadInt32()
                $returnedSequence = $response.ReadInt64()
                $androidReceiveNs = $response.ReadInt64()
                $androidSendNs = $response.ReadInt64()
            } finally {
                $response.Dispose()
                $responseStream.Dispose()
            }
            if ($magic -ne $syncMagic -or $version -ne $syncVersion -or $returnedSequence -ne $sequence) {
                throw "Invalid monotonic clock synchronization response for sample $sequence."
            }
            $roundTripNs = $windowsReceiveNs - $windowsSendNs
            $androidProcessingNs = [double]($androidSendNs - $androidReceiveNs)
            $networkRoundTripNs = [math]::Max(0.0, $roundTripNs - $androidProcessingNs)
            $offsetNs = (($windowsSendNs + $windowsReceiveNs) / 2.0) -
                (($androidReceiveNs + $androidSendNs) / 2.0)
            $results.Add([pscustomobject]@{
                sequence = $sequence
                offsetWindowsMinusAndroidNs = $offsetNs
                networkRoundTripNs = $networkRoundTripNs
                uncertaintyNs = $networkRoundTripNs / 2.0
            })
        }
    } finally {
        $writer.Dispose()
        $reader.Dispose()
    }
    $best = $results | Sort-Object networkRoundTripNs | Select-Object -First 1
    [pscustomobject]@{
        samples = $results.Count
        offsetWindowsMinusAndroidNs = [double]$best.offsetWindowsMinusAndroidNs
        bestNetworkRoundTripMs = [double]$best.networkRoundTripNs / 1000000.0
        uncertaintyMs = [double]$best.uncertaintyNs / 1000000.0
    }
}

function Get-LatencyStatistics {
    param(
        [Parameter(Mandatory)] [System.Collections.IEnumerable]$Measurements,
        [Parameter(Mandatory)] [string]$Property
    )

    $values = @($Measurements | ForEach-Object { $_.$Property } | Where-Object { $null -ne $_ } | Sort-Object)
    if ($values.Count -eq 0) {
        return [pscustomobject]@{ samples = 0; averageMs = $null; medianMs = $null; maximumMs = $null }
    }
    $sum = 0.0
    foreach ($value in $values) { $sum += [double]$value }
    $middle = [math]::Floor($values.Count / 2)
    $median = if ($values.Count % 2 -eq 0) {
        ([double]$values[$middle - 1] + [double]$values[$middle]) / 2.0
    } else {
        [double]$values[$middle]
    }
    [pscustomobject]@{
        samples = $values.Count
        averageMs = [math]::Round($sum / $values.Count, 3)
        medianMs = [math]::Round($median, 3)
        maximumMs = [math]::Round([double]$values[-1], 3)
    }
}

function Connect-VideoSocket {
    param([int]$TimeoutSeconds)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = [System.Net.Sockets.TcpClient]::new()
        try {
            $candidate.NoDelay = $true
            $candidate.ReceiveTimeout = 10000
            $connectTask = $candidate.ConnectAsync('127.0.0.1', 27184)
            if (-not $connectTask.Wait(1000)) { throw 'connection timed out' }
            if ($connectTask.IsFaulted) { throw $connectTask.Exception.GetBaseException() }
            return $candidate
        } catch {
            $lastError = $_.Exception.Message
            $candidate.Dispose()
            Start-Sleep -Milliseconds 300
        }
    }
    throw "The Android video server did not become available on port 27184: $lastError. Keep the app open and unlocked."
}

function Start-AndroidCameraActivity {
    param([Parameter(Mandatory)] [string]$AdbPath)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $AdbPath
    $startInfo.Arguments = 'shell am start -n com.androidcam.inspector/.MainActivity'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw 'ADB process could not be started.' }
        $standardOutputTask = $process.StandardOutput.ReadToEndAsync()
        $standardErrorTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        [pscustomobject]@{
            exitCode = $process.ExitCode
            standardOutput = $standardOutputTask.GetAwaiter().GetResult()
            standardError = $standardErrorTask.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

$selectedDecoder = Find-Decoder -RequestedDecoder $Decoder
$deviceLines = & $adb devices
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed.' }
$unauthorized = @($deviceLines | Where-Object { $_ -match "^\S+\s+unauthorized$" })
if ($unauthorized.Count -gt 0) {
    throw 'The phone is unauthorized. Accept the USB debugging authorization on the phone and retry.'
}
$authorizedDevices = @($deviceLines | Where-Object { $_ -match "^\S+\s+device$" })
if ($authorizedDevices.Count -eq 0) { throw 'No authorized Android device is connected through ADB.' }
if ($authorizedDevices.Count -gt 1) { throw 'More than one authorized Android device is connected; connect only the target phone.' }

$forwardResult = & $adb forward 'tcp:27184' 'tcp:27184'
if ($LASTEXITCODE -ne 0) {
    throw "Could not forward local port 27184. It may already be occupied: $forwardResult"
}
$forwardConfigured = $true

$launchResult = Start-AndroidCameraActivity -AdbPath $adb
$launchText = @($launchResult.standardOutput, $launchResult.standardError) -join ' '
if ($launchResult.exitCode -ne 0 -or $launchText -match '(?m)^\s*(Error type|Error:|Exception)|does not exist') {
    & $adb forward --remove 'tcp:27184' 2>$null | Out-Null
    $forwardConfigured = $false
    throw "Could not start the Android camera app (ADB exit code $($launchResult.exitCode)): $($launchText.Trim())"
}
if (-not [string]::IsNullOrWhiteSpace($launchResult.standardError)) {
    Write-Verbose "adb shell am start stderr: $($launchResult.standardError.Trim())"
} else {
    # A cold Activity start can return before its loopback video server is bound.
    Start-Sleep -Milliseconds 750
}

$decoderArguments = if ($selectedDecoder.name -eq 'FFplay') {
    @(
        '-hide_banner', '-loglevel', 'warning',
        '-f', 'h264', '-avioflags', 'direct', '-fflags', 'nobuffer', '-flags', 'low_delay',
        '-analyzeduration', '0', '-probesize', '32', '-framedrop',
        '-sync', 'video',
        '-window_title', 'StratoCam - USB 1080p30', '-i', 'pipe:0'
    )
} else {
    @(
        '--intf=dummy', '--no-one-instance', '--no-audio', '--no-video-title-show', '--play-and-exit',
        '--demux=h264', "--file-caching=$VlcCachingMs", "--live-caching=$VlcCachingMs",
        "--network-caching=$VlcCachingMs", '--clock-jitter=0', '--clock-synchro=0',
        '--drop-late-frames', '--skip-frames', '-'
    )
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $selectedDecoder.path
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $false
$startInfo.RedirectStandardInput = $true
$startInfo.Arguments = ($decoderArguments | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
}) -join ' '

$videoMagic = 0x31564341
$protocolVersion = 3
$videoHeaderSize = 80
$flagCodecConfig = 1
$flagKeyFrame = 2
$flagStreamInfo = 4
$client = $null
$reader = $null
$decoderProcess = $null
$decoderInput = $null
$diagnosticCopy = $null
$metadata = $null
$frames = 0L
$keyFrames = 0L
$streamBytes = 0L
$accessUnits = 0L
$decoderClosed = $false
$firstAccessUnitDeliveredUtc = $null
$lastAccessUnitDeliveredUtc = $null
$lastAccessUnitDeliveryTicks = $null
$maximumAccessUnitDeliveryGapMs = 0.0
$decoderCpuSeconds = $null
$clockSynchronization = $null
$latencyMeasurements = [System.Collections.Generic.List[object]]::new()
$timer = [System.Diagnostics.Stopwatch]::new()

try {
    $client = Connect-VideoSocket -TimeoutSeconds 10
    $networkStream = $client.GetStream()
    $clockSynchronization = Invoke-ClockSynchronization -Stream $networkStream
    Write-Host ("Clock sync: best RTT {0:N3} ms, one-way uncertainty +/-{1:N3} ms" -f
        $clockSynchronization.bestNetworkRoundTripMs, $clockSynchronization.uncertaintyMs)
    $reader = [System.IO.BinaryReader]::new($networkStream, [System.Text.Encoding]::UTF8, $true)
    if (-not [string]::IsNullOrWhiteSpace($DiagnosticCopyPath)) {
        $copyPath = if ([System.IO.Path]::IsPathRooted($DiagnosticCopyPath)) {
            [System.IO.Path]::GetFullPath($DiagnosticCopyPath)
        } else {
            [System.IO.Path]::GetFullPath((Join-Path $projectRoot $DiagnosticCopyPath))
        }
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $copyPath)) | Out-Null
        $diagnosticCopy = [System.IO.File]::Open($copyPath, [System.IO.FileMode]::Create)
    }

    $decoderProcess = [System.Diagnostics.Process]::new()
    $decoderProcess.StartInfo = $startInfo
    if (-not $decoderProcess.Start()) { throw "Could not start $($selectedDecoder.name)." }
    $decoderInput = $decoderProcess.StandardInput.BaseStream
    Write-Host "Live preview started with $($selectedDecoder.name). Close its window or press Ctrl+C here to stop."
    $timer.Start()

    while ($DurationSeconds -eq 0 -or $timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        if ($decoderProcess.HasExited) {
            $decoderClosed = $true
            break
        }
        $headerBytes = Read-Exactly -Reader $reader -Count $videoHeaderSize
        $headerStream = [System.IO.MemoryStream]::new($headerBytes, $false)
        $header = [System.IO.BinaryReader]::new($headerStream)
        try {
            $magic = $header.ReadInt32()
            $version = $header.ReadInt32()
            $flags = $header.ReadInt32()
            $payloadSize = $header.ReadInt32()
            $presentationTimeUs = $header.ReadInt64()
            $frameNumber = $header.ReadInt64()
            $captureTimestampNs = $header.ReadInt64()
            $captureCallbackTimestampNs = $header.ReadInt64()
            $encoderOutputTimestampNs = $header.ReadInt64()
            $androidSendTimestampNs = $header.ReadInt64()
            $accessUnitSequence = $header.ReadInt64()
            $encodedQueueDepth = $header.ReadInt32()
            $queueRecoveryCount = $header.ReadInt32()
        } finally {
            $header.Dispose()
            $headerStream.Dispose()
        }
        if ($magic -ne $videoMagic) { throw ('Invalid video packet magic: 0x{0:X8}' -f $magic) }
        if ($version -ne $protocolVersion) { throw "Unsupported video protocol version: $version" }
        if ($payloadSize -lt 0 -or $payloadSize -gt 16MB) { throw "Invalid video payload size: $payloadSize" }
        $payload = Read-Exactly -Reader $reader -Count $payloadSize
        $windowsReceiveTimestampNs = Get-MonotonicNanoseconds

        if (($flags -band $flagStreamInfo) -ne 0) {
            $metadata = [System.Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json
            Write-Host ("Stream: {0} {1}x{2}@{3}, sensor orientation {4} degrees" -f
                $metadata.codec, $metadata.width, $metadata.height, $metadata.fps, $metadata.sensorOrientationDegrees)
            continue
        }

        try {
            $decoderInput.Write($payload, 0, $payload.Length)
            $decoderInput.Flush()
            $windowsDeliveryTimestampNs = Get-MonotonicNanoseconds
            $deliveredUtc = [DateTime]::UtcNow
            $deliveredTicks = [System.Diagnostics.Stopwatch]::GetTimestamp()
            if ($null -eq $firstAccessUnitDeliveredUtc) { $firstAccessUnitDeliveredUtc = $deliveredUtc }
            if ($null -ne $lastAccessUnitDeliveryTicks) {
                $deliveryGapMs = (($deliveredTicks - $lastAccessUnitDeliveryTicks) * 1000.0) / [System.Diagnostics.Stopwatch]::Frequency
                if ($deliveryGapMs -gt $maximumAccessUnitDeliveryGapMs) { $maximumAccessUnitDeliveryGapMs = $deliveryGapMs }
            }
            $lastAccessUnitDeliveredUtc = $deliveredUtc
            $lastAccessUnitDeliveryTicks = $deliveredTicks
            $accessUnits += 1
        } catch {
            if ($decoderProcess.HasExited) {
                $decoderClosed = $true
                break
            }
            throw "The decoder stopped accepting video data: $($_.Exception.Message)"
        }
        if ($null -ne $diagnosticCopy) { $diagnosticCopy.Write($payload, 0, $payload.Length) }
        $streamBytes += $payload.Length
        if (($flags -band $flagCodecConfig) -eq 0) {
            $frames += 1
            if (($flags -band $flagKeyFrame) -ne 0) { $keyFrames += 1 }
            $offsetNs = $clockSynchronization.offsetWindowsMinusAndroidNs
            $captureAvailable = $captureTimestampNs -gt 0
            $encoderComparable = $encoderOutputTimestampNs -gt 0
            $captureToEncoderMs = if ($captureAvailable -and $encoderComparable) {
                ($encoderOutputTimestampNs - $captureTimestampNs) / 1000000.0
            } else { $null }
            $encoderToReceiveMs = if ($encoderComparable) {
                ($windowsReceiveTimestampNs - ($encoderOutputTimestampNs + $offsetNs)) / 1000000.0
            } else { $null }
            $captureToDeliveryMs = if ($captureAvailable) {
                ($windowsDeliveryTimestampNs - ($captureTimestampNs + $offsetNs)) / 1000000.0
            } else { $null }
            $latencyMeasurements.Add([pscustomobject]@{
                frameNumber = $frameNumber
                presentationTimeUs = $presentationTimeUs
                captureTimestampNsAndroid = if ($captureTimestampNs -gt 0) { $captureTimestampNs } else { $null }
                captureCallbackTimestampNsAndroid = if ($captureCallbackTimestampNs -gt 0) {
                    $captureCallbackTimestampNs
                } else { $null }
                encoderOutputTimestampNsAndroid = if ($encoderComparable) { $encoderOutputTimestampNs } else { $null }
                androidSendTimestampNs = $androidSendTimestampNs
                windowsReceiveTimestampNs = [math]::Round($windowsReceiveTimestampNs, 0)
                windowsVlcStdinTimestampNs = [math]::Round($windowsDeliveryTimestampNs, 0)
                captureToEncoderMs = $captureToEncoderMs
                captureCallbackToEncoderMs = if ($captureCallbackTimestampNs -gt 0 -and $encoderComparable) {
                    ($encoderOutputTimestampNs - $captureCallbackTimestampNs) / 1000000.0
                } else { $null }
                encoderToWindowsReceiveMs = $encoderToReceiveMs
                windowsReceiveToVlcStdinMs = ($windowsDeliveryTimestampNs - $windowsReceiveTimestampNs) / 1000000.0
                encoderToAndroidSendMs = if ($encoderComparable -and $androidSendTimestampNs -gt 0) {
                    ($androidSendTimestampNs - $encoderOutputTimestampNs) / 1000000.0
                } else { $null }
                androidSendToWindowsReceiveMs = if ($androidSendTimestampNs -gt 0) {
                    ($windowsReceiveTimestampNs - ($androidSendTimestampNs + $offsetNs)) / 1000000.0
                } else { $null }
                captureToVlcStdinMs = $captureToDeliveryMs
            })
        }
    }
} finally {
    $timer.Stop()
    if ($null -ne $diagnosticCopy) { $diagnosticCopy.Dispose() }
    if ($null -ne $decoderInput) { try { $decoderInput.Dispose() } catch {} }
    if ($null -ne $reader) { $reader.Dispose() }
    if ($null -ne $client) { $client.Dispose() }
    if ($null -ne $decoderProcess) {
        if (-not $decoderProcess.WaitForExit(2000)) {
            [void]$decoderProcess.CloseMainWindow()
            if (-not $decoderProcess.WaitForExit(2000)) { $decoderProcess.Kill() }
        }
        try { $decoderCpuSeconds = $decoderProcess.TotalProcessorTime.TotalSeconds } catch {}
        $decoderProcess.Dispose()
    }
    if ($forwardConfigured) {
        & $adb forward --remove 'tcp:27184' 2>$null | Out-Null
        $forwardConfigured = $false
    }
}

$elapsed = $timer.Elapsed.TotalSeconds
if ($frames -eq 0) {
    if ($decoderClosed) { throw "$($selectedDecoder.name) closed before receiving a video frame." }
    throw 'No H.264 video frames reached the decoder.'
}

$resolvedLatencyCsvPath = if ([string]::IsNullOrWhiteSpace($LatencyCsvPath)) {
    Join-Path $projectRoot ("captures\latency-{0}.csv" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
} elseif ([System.IO.Path]::IsPathRooted($LatencyCsvPath)) {
    [System.IO.Path]::GetFullPath($LatencyCsvPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $LatencyCsvPath))
}
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $resolvedLatencyCsvPath)) | Out-Null
$latencyMeasurements | Export-Csv -LiteralPath $resolvedLatencyCsvPath -NoTypeInformation -Encoding UTF8

$latencySummary = [pscustomobject]@{
    captureToEncoder = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'captureToEncoderMs'
    captureCallbackToEncoder = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'captureCallbackToEncoderMs'
    encoderOutputToWindowsReceive = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'encoderToWindowsReceiveMs'
    windowsReceiveToVlcStdin = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'windowsReceiveToVlcStdinMs'
    captureToVlcStdin = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'captureToVlcStdinMs'
    encoderOutputToAndroidSend = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'encoderToAndroidSendMs'
    androidSendToWindowsReceive = Get-LatencyStatistics -Measurements $latencyMeasurements -Property 'androidSendToWindowsReceiveMs'
}

[pscustomobject]@{
    decoder = $selectedDecoder.name
    codec = $metadata.codec
    width = $metadata.width
    height = $metadata.height
    requestedFps = $metadata.fps
    durationSeconds = [math]::Round($elapsed, 3)
    framesSentToDecoder = $frames
    keyFrames = $keyFrames
    bytesSentToDecoder = $streamBytes
    accessUnitsSentToDecoder = $accessUnits
    feedFps = [math]::Round($frames / $elapsed, 2)
    feedMbps = [math]::Round(($streamBytes * 8) / $elapsed / 1000000, 2)
    firstAccessUnitDeliveredUtc = $firstAccessUnitDeliveredUtc.ToString('o')
    lastAccessUnitDeliveredUtc = $lastAccessUnitDeliveredUtc.ToString('o')
    maximumAccessUnitDeliveryGapMs = [math]::Round($maximumAccessUnitDeliveryGapMs, 2)
    decoderCpuSeconds = if ($null -eq $decoderCpuSeconds) { $null } else { [math]::Round($decoderCpuSeconds, 3) }
    decoderCpuPercentOfOneCore = if ($null -eq $decoderCpuSeconds -or $elapsed -le 0) {
        $null
    } else {
        [math]::Round(($decoderCpuSeconds / $elapsed) * 100, 2)
    }
    captureTimestampSource = $metadata.captureTimestampSource
    captureTimestampComparableToMonotonicClock = $metadata.captureTimestampSource -eq 'REALTIME'
    correlatedCaptureFrames = @($latencyMeasurements | Where-Object { $_.frameNumber -ge 0 }).Count
    clockSyncSamples = $clockSynchronization.samples
    clockSyncBestRoundTripMs = [math]::Round($clockSynchronization.bestNetworkRoundTripMs, 3)
    clockSyncUncertaintyMs = [math]::Round($clockSynchronization.uncertaintyMs, 3)
    latencyCsvPath = $resolvedLatencyCsvPath
    latency = $latencySummary
    decoderClosedByUser = $decoderClosed
}
