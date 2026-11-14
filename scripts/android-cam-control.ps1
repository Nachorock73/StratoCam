[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('ping', 'get-state', 'get-capabilities', 'get-device-status', 'set-video-mode', 'set-exposure', 'set-focus', 'set-wb', 'set-zoom', 'reset-auto')]
    [string]$Command = 'get-state',

    [ValidateSet('Auto', 'Manual')]
    [string]$Mode = 'Manual',

    [int]$Iso,
    [long]$ExposureTimeNs,
    [single]$FocusDistanceDiopters,
    [single]$RedGain,
    [single]$BlueGain,
    [single]$ZoomRatio,
    [ValidateSet('1080p30', '4k30')]
    [string]$VideoMode = '1080p30',
    [ValidateSet(8000000, 16000000, 20000000, 25000000, 30000000, 40000000)]
    [int]$Bitrate = 8000000
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $projectRoot '.tools\android-sdk\platform-tools\adb.exe'
if (-not (Test-Path -LiteralPath $adb)) {
    throw "ADB not found at $adb"
}

$projectAdbKey = Join-Path $projectRoot '.tools\adb-keys\adbkey'
if (Test-Path -LiteralPath $projectAdbKey) {
    $env:ADB_VENDOR_KEYS = $projectAdbKey
}

$deviceLines = & $adb devices
if ($LASTEXITCODE -ne 0) { throw 'adb devices failed.' }
$authorizedDevices = @($deviceLines | Where-Object { $_ -match "^\S+\s+device$" })
if ($authorizedDevices.Count -ne 1) {
    throw "Expected exactly one authorized Android device; found $($authorizedDevices.Count)."
}

$forwardResult = & $adb forward 'tcp:27183' 'tcp:27183'
if ($LASTEXITCODE -ne 0) { throw "adb forward failed: $forwardResult" }

$params = @{}
switch ($Command) {
    'set-video-mode' {
        $params.width = if ($VideoMode -eq '4k30') { 3840 } else { 1920 }
        $params.height = if ($VideoMode -eq '4k30') { 2160 } else { 1080 }
        $params.fps = 30
        $params.bitrate = $Bitrate
    }
    'set-exposure' {
        $params.auto = $Mode -eq 'Auto'
        if ($PSBoundParameters.ContainsKey('Iso')) { $params.iso = $Iso }
        if ($PSBoundParameters.ContainsKey('ExposureTimeNs')) { $params.exposureTimeNs = $ExposureTimeNs }
    }
    'set-focus' {
        $params.auto = $Mode -eq 'Auto'
        if ($PSBoundParameters.ContainsKey('FocusDistanceDiopters')) {
            $params.focusDistanceDiopters = $FocusDistanceDiopters
        }
    }
    'set-wb' {
        $params.auto = $Mode -eq 'Auto'
        if ($PSBoundParameters.ContainsKey('RedGain')) { $params.redGain = $RedGain }
        if ($PSBoundParameters.ContainsKey('BlueGain')) { $params.blueGain = $BlueGain }
    }
    'set-zoom' {
        if (-not $PSBoundParameters.ContainsKey('ZoomRatio')) { throw 'set-zoom requires -ZoomRatio.' }
        $params.zoomRatio = $ZoomRatio
    }
}

$wireCommand = $Command.Replace('-', '_')
if ($wireCommand -eq 'set_wb') { $wireCommand = 'set_white_balance' }
$request = @{
    version = 1
    id = [guid]::NewGuid().ToString('N')
    command = $wireCommand
    params = $params
} | ConvertTo-Json -Compress

$client = [System.Net.Sockets.TcpClient]::new()
try {
    $client.Connect('127.0.0.1', 27183)
    $stream = $client.GetStream()
    $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false), 1024, $true)
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8, $false, 1024, $true)
    $writer.NewLine = "`n"
    $writer.WriteLine($request)
    $writer.Flush()
    $responseLine = $reader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($responseLine)) { throw 'The phone returned an empty response.' }
    $response = $responseLine | ConvertFrom-Json
    if (-not $response.ok) { throw "Phone rejected the command: $($response.message)" }
    $response
} finally {
    if ($null -ne $reader) { $reader.Dispose() }
    if ($null -ne $writer) { $writer.Dispose() }
    $client.Dispose()
}
