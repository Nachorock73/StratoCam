[CmdletBinding()]
param(
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 10,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $projectRoot '.tools\android-sdk\platform-tools\adb.exe'
if (-not (Test-Path -LiteralPath $adb)) { throw "ADB not found at $adb" }

$projectAdbKey = Join-Path $projectRoot '.tools\adb-keys\adbkey'
if (Test-Path -LiteralPath $projectAdbKey) { $env:ADB_VENDOR_KEYS = $projectAdbKey }

$deviceLines = & $adb devices
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed.' }
$authorizedDevices = @($deviceLines | Where-Object { $_ -match "^\S+\s+device$" })
if ($authorizedDevices.Count -ne 1) {
    throw "Expected exactly one authorized Android device; found $($authorizedDevices.Count)."
}

$forwardResult = & $adb forward 'tcp:27184' 'tcp:27184'
if ($LASTEXITCODE -ne 0) { throw "adb forward failed: $forwardResult" }

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path 'captures' ("stratocam-{0}.h264" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$fullOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $OutputPath))
}
$outputDirectory = Split-Path -Parent $fullOutputPath
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

function Read-Exactly {
    param(
        [Parameter(Mandatory)] [System.IO.BinaryReader]$Reader,
        [Parameter(Mandatory)] [int]$Count
    )
    $bytes = $Reader.ReadBytes($Count)
    if ($bytes.Length -ne $Count) { throw "Video connection closed after $($bytes.Length) of $Count expected bytes." }
    $bytes
}

function Initialize-ClockSynchronization {
    param([Parameter(Mandatory)] [System.Net.Sockets.NetworkStream]$Stream)

    $syncMagic = 0x31534341
    $syncVersion = 1
    $writer = [System.IO.BinaryWriter]::new($Stream, [System.Text.Encoding]::UTF8, $true)
    $reader = [System.IO.BinaryReader]::new($Stream, [System.Text.Encoding]::UTF8, $true)
    try {
        for ($sequence = 0; $sequence -lt 9; $sequence += 1) {
            $writer.Write([int]$syncMagic)
            $writer.Write([int]$syncVersion)
            $writer.Write([long]$sequence)
            $writer.Flush()
            $responseBytes = Read-Exactly -Reader $reader -Count 32
            $responseStream = [System.IO.MemoryStream]::new($responseBytes, $false)
            $response = [System.IO.BinaryReader]::new($responseStream)
            try {
                $magic = $response.ReadInt32()
                $version = $response.ReadInt32()
                $returnedSequence = $response.ReadInt64()
                [void]$response.ReadInt64()
                [void]$response.ReadInt64()
            } finally {
                $response.Dispose()
                $responseStream.Dispose()
            }
            if ($magic -ne $syncMagic -or $version -ne $syncVersion -or $returnedSequence -ne $sequence) {
                throw "Invalid clock synchronization response for sample $sequence."
            }
        }
    } finally {
        $writer.Dispose()
        $reader.Dispose()
    }
}

$videoMagic = 0x31564341
$protocolVersion = 3
$videoHeaderSize = 80
$flagCodecConfig = 1
$flagKeyFrame = 2
$flagStreamInfo = 4
$packets = 0L
$frames = 0L
$keyFrames = 0L
$streamBytes = 0L
$metadata = $null
$client = [System.Net.Sockets.TcpClient]::new()
$file = $null
$reader = $null
$timer = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $client.ReceiveTimeout = 10000
    $client.NoDelay = $true
    $client.Connect('127.0.0.1', 27184)
    $networkStream = $client.GetStream()
    Initialize-ClockSynchronization -Stream $networkStream
    $reader = [System.IO.BinaryReader]::new($networkStream, [System.Text.Encoding]::UTF8, $true)
    $file = [System.IO.File]::Open($fullOutputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    $timer.Restart()

    while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
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
        $packets += 1

        if (($flags -band $flagStreamInfo) -ne 0) {
            $metadata = [System.Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json
            continue
        }
        if (($flags -band $flagCodecConfig) -ne 0) {
            $file.Write($payload, 0, $payload.Length)
            $streamBytes += $payload.Length
            continue
        }

        $file.Write($payload, 0, $payload.Length)
        $streamBytes += $payload.Length
        $frames += 1
        if (($flags -band $flagKeyFrame) -ne 0) { $keyFrames += 1 }
    }
} finally {
    $timer.Stop()
    if ($null -ne $file) { $file.Dispose() }
    if ($null -ne $reader) { $reader.Dispose() }
    $client.Dispose()
}

if ($frames -eq 0 -or $streamBytes -eq 0) { throw 'No encoded H.264 frames were received.' }
$elapsed = $timer.Elapsed.TotalSeconds
[pscustomobject]@{
    outputPath = $fullOutputPath
    codec = $metadata.codec
    width = $metadata.width
    height = $metadata.height
    requestedFps = $metadata.fps
    sensorOrientationDegrees = $metadata.sensorOrientationDegrees
    captureTimestampSource = $metadata.captureTimestampSource
    requestedBitrate = $metadata.bitrate
    durationSeconds = [math]::Round($elapsed, 3)
    packets = $packets
    frames = $frames
    keyFrames = $keyFrames
    bytes = $streamBytes
    receivedFps = [math]::Round($frames / $elapsed, 2)
    receivedMbps = [math]::Round(($streamBytes * 8) / $elapsed / 1000000, 2)
}
