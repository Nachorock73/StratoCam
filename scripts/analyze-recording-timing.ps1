[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$RecordingPath,
    [string]$VideoMetricsPath,
    [string]$AudioMetricsPath,
    [string]$OutputPath,
    [ValidateRange(1, 240)]
    [int]$RequestedFps = 30,
    [string]$FfprobePath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$recording = [IO.Path]::GetFullPath($RecordingPath)
if (-not (Test-Path -LiteralPath $recording -PathType Leaf)) { throw "Recording not found: $recording" }
if (-not $FfprobePath) { $FfprobePath = Join-Path $root '.tools\ffmpeg\bin\ffprobe.exe' }
if (-not (Test-Path -LiteralPath $FfprobePath -PathType Leaf)) { throw "ffprobe not found: $FfprobePath" }

$baseName = [IO.Path]::GetFileNameWithoutExtension($recording)
if (-not $VideoMetricsPath) {
    $candidate = Join-Path $root "captures\direct-recording-$baseName.csv"
    if (Test-Path -LiteralPath $candidate) { $VideoMetricsPath = $candidate }
}
if (-not $AudioMetricsPath) {
    $candidate = Join-Path $root "captures\direct-recording-$baseName-audio.csv"
    if (Test-Path -LiteralPath $candidate) { $AudioMetricsPath = $candidate }
}
if (-not $OutputPath) { $OutputPath = Join-Path $root "captures\recording-timing-$baseName.json" }

function Invoke-Probe([string[]]$Arguments) {
    $text = (& $FfprobePath -v error @Arguments -- $recording) | Out-String
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for $recording" }
    $text | ConvertFrom-Json
}

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count) - 1
    $sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
}

function Get-Stats([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    [ordered]@{
        count = $Values.Count
        minimum = [Math]::Round(($Values | Measure-Object -Minimum).Minimum, 6)
        p50 = [Math]::Round((Get-Percentile $Values 50), 6)
        p95 = [Math]::Round((Get-Percentile $Values 95), 6)
        p99 = [Math]::Round((Get-Percentile $Values 99), 6)
        maximum = [Math]::Round(($Values | Measure-Object -Maximum).Maximum, 6)
    }
}

function Parse-TimeBase([string]$Value) {
    $parts = $Value.Split('/')
    if ($parts.Count -ne 2) { throw "Invalid time base: $Value" }
    [ordered]@{ numerator = [long]$parts[0]; denominator = [long]$parts[1] }
}

function Get-VideoTiming($Packets, $Stream, [int]$Fps) {
    $timeBase = Parse-TimeBase $Stream.time_base
    $pts = @($Packets | ForEach-Object { [long]$_.pts })
    $dts = @($Packets | ForEach-Object { [long]$_.dts })
    $durations = @($Packets | ForEach-Object { if ($null -ne $_.duration) { [long]$_.duration } else { 0L } })
    $intervalMs = [Collections.Generic.List[double]]::new()
    $slotCounts = [Collections.Generic.List[long]]::new()
    $duplicatePts = 0
    $nonMonotonicPts = 0
    $nonMonotonicDts = 0
    $nominalIntervals = 0
    $twoSlotOrLarger = 0
    $missingSlots = 0L
    $maxIdealErrorMs = 0.0
    $oneMinuteErrorMs = $null
    $fiveMinuteErrorMs = $null
    for ($index = 0; $index -lt $pts.Count; ++$index) {
        $elapsedNumerator = [decimal](($pts[$index] - $pts[0]) * $timeBase.numerator)
        $elapsedSeconds = $elapsedNumerator / [decimal]$timeBase.denominator
        $idealSeconds = [decimal]$index / [decimal]$Fps
        $errorMs = [double](($elapsedSeconds - $idealSeconds) * 1000)
        $maxIdealErrorMs = [Math]::Max($maxIdealErrorMs, [Math]::Abs($errorMs))
        if ($index -eq 60 * $Fps) { $oneMinuteErrorMs = [Math]::Round($errorMs, 6) }
        if ($index -eq 300 * $Fps) { $fiveMinuteErrorMs = [Math]::Round($errorMs, 6) }
        if ($index -eq 0) { continue }
        $delta = $pts[$index] - $pts[$index - 1]
        if ($delta -eq 0) { ++$duplicatePts }
        if ($delta -lt 0) { ++$nonMonotonicPts }
        if ($dts[$index] -lt $dts[$index - 1]) { ++$nonMonotonicDts }
        $deltaMs = [double]([decimal]$delta * $timeBase.numerator * 1000 / $timeBase.denominator)
        $intervalMs.Add($deltaMs)
        $slots = [long][Math]::Round($deltaMs * $Fps / 1000.0, [MidpointRounding]::AwayFromZero)
        $slotCounts.Add($slots)
        if ($slots -eq 1) { ++$nominalIntervals }
        if ($slots -ge 2) { ++$twoSlotOrLarger; $missingSlots += $slots - 1 }
    }
    $lastDuration = if ($durations.Count) { $durations[-1] } else { 0L }
    $ptsDurationSeconds = if ($pts.Count) {
        [double]([decimal](($pts[-1] - $pts[0] + $lastDuration) * $timeBase.numerator) / $timeBase.denominator)
    } else { 0.0 }
    $expectedDurationSeconds = $pts.Count / [double]$Fps
    $totalIdealErrorMs = ($ptsDurationSeconds - $expectedDurationSeconds) * 1000.0
    $durationMinutes = $expectedDurationSeconds / 60.0
    $slope = if ($durationMinutes -gt 0) { $totalIdealErrorMs / $durationMinutes } else { $null }
    $oneTickMs = [double]$timeBase.numerator * 1000.0 / $timeBase.denominator
    $trueCfr = $duplicatePts -eq 0 -and $nonMonotonicPts -eq 0 -and $missingSlots -eq 0 -and
        $maxIdealErrorMs -le ($oneTickMs + 0.000001)
    $nominalMetadata = $Stream.r_frame_rate -eq "$Fps/1"
    $status = if ($trueCfr) { "TRUE CFR $Fps/1" }
        elseif ($nominalMetadata) { 'nominal CFR metadata with irregular underlying timestamps' }
        else { 'VFR / irregular timing' }
    [ordered]@{
        codec = $Stream.codec_name
        profile = $Stream.profile
        resolution = "$($Stream.width)x$($Stream.height)"
        requestedFps = $Fps
        rFrameRate = $Stream.r_frame_rate
        avgFrameRate = $Stream.avg_frame_rate
        timeBase = $Stream.time_base
        frameCount = $pts.Count
        firstPts = if ($pts.Count) { $pts[0] } else { $null }
        lastPts = if ($pts.Count) { $pts[-1] } else { $null }
        ptsDerivedDurationSeconds = [Math]::Round($ptsDurationSeconds, 6)
        expectedDurationSeconds = [Math]::Round($expectedDurationSeconds, 6)
        expectedMinusPtsDurationMs = [Math]::Round(($expectedDurationSeconds - $ptsDurationSeconds) * 1000, 6)
        intervalMs = Get-Stats $intervalMs.ToArray()
        duplicatePts = $duplicatePts
        nonMonotonicPts = $nonMonotonicPts
        nonMonotonicDts = $nonMonotonicDts
        nominalOneSlotIntervals = $nominalIntervals
        twoSlotOrLargerIntervals = $twoSlotOrLarger
        missingFrameSlots = $missingSlots
        intervalSlotDistribution = @($slotCounts | Group-Object | Sort-Object { [long]$_.Name } | ForEach-Object {
            @{ slots = [long]$_.Name; count = $_.Count }
        })
        timingErrorAtOneMinuteMs = $oneMinuteErrorMs
        timingErrorAtFiveMinutesMs = $fiveMinuteErrorMs
        timingErrorSlopeMsPerMinute = if ($null -ne $slope) { [Math]::Round($slope, 6) } else { $null }
        maximumIdealSlotDeviationMs = [Math]::Round($maxIdealErrorMs, 6)
        apparentRepeatedFrames = 'not assessed; timestamp analysis does not decode or hash image content'
        cfrStatus = $status
    }
}

function Get-AudioTiming($Packets, $Frames, $Stream) {
    if (-not $Stream) { return $null }
    $timeBase = Parse-TimeBase $Stream.time_base
    $pts = @($Packets | ForEach-Object { [long]$_.pts })
    $dts = @($Packets | ForEach-Object { [long]$_.dts })
    $durations = @($Packets | ForEach-Object { [long]$_.duration })
    $durationMs = @($durations | ForEach-Object {
        [double]([decimal]$_ * $timeBase.numerator * 1000 / $timeBase.denominator)
    })
    $discontinuities = 0
    $nonMonotonicPts = 0
    $nonMonotonicDts = 0
    for ($index = 1; $index -lt $pts.Count; ++$index) {
        if ($pts[$index] -ne $pts[$index - 1] + $durations[$index - 1]) { ++$discontinuities }
        if ($pts[$index] -lt $pts[$index - 1]) { ++$nonMonotonicPts }
        if ($dts[$index] -lt $dts[$index - 1]) { ++$nonMonotonicDts }
    }
    $decodedSamples = [long](($Frames | Measure-Object -Property nb_samples -Sum).Sum)
    $lastDuration = if ($durations.Count) { $durations[-1] } else { 0L }
    $timelineDuration = if ($pts.Count) {
        [double]([decimal](($pts[-1] - $pts[0] + $lastDuration) * $timeBase.numerator) / $timeBase.denominator)
    } else { 0.0 }
    [ordered]@{
        codec = $Stream.codec_name
        profile = $Stream.profile
        bitrate = if ($Stream.bit_rate) { [long]$Stream.bit_rate } else { $null }
        sampleRate = [int]$Stream.sample_rate
        channels = [int]$Stream.channels
        channelLayout = $Stream.channel_layout
        timeBase = $Stream.time_base
        packetCount = $pts.Count
        firstPts = if ($pts.Count) { $pts[0] } else { $null }
        lastPts = if ($pts.Count) { $pts[-1] } else { $null }
        decodedSampleCount = $decodedSamples
        sampleCountDurationSeconds = [Math]::Round($decodedSamples / [double]$Stream.sample_rate, 6)
        timestampDurationSeconds = [Math]::Round($timelineDuration, 6)
        packetDurationMs = Get-Stats $durationMs
        timestampDiscontinuities = $discontinuities
        nonMonotonicPts = $nonMonotonicPts
        nonMonotonicDts = $nonMonotonicDts
    }
}

$metadata = Invoke-Probe @('-show_streams', '-show_format', '-of', 'json')
$packetData = Invoke-Probe @('-show_packets', '-show_entries', 'packet=stream_index,pts,dts,duration,flags', '-of', 'json')
$videoStream = @($metadata.streams | Where-Object codec_type -eq 'video')[0]
$audioStream = @($metadata.streams | Where-Object codec_type -eq 'audio')[0]
$videoPackets = @($packetData.packets | Where-Object stream_index -eq $videoStream.index)
$audioPackets = if ($audioStream) { @($packetData.packets | Where-Object stream_index -eq $audioStream.index) } else { @() }
$audioFrames = if ($audioStream) {
    @(Invoke-Probe @('-select_streams', 'a:0', '-show_frames', '-show_entries', 'frame=pts,nb_samples', '-of', 'json')).frames
} else { @() }

$video = Get-VideoTiming $videoPackets $videoStream $RequestedFps
$audio = Get-AudioTiming $audioPackets $audioFrames $audioStream
$sourceVideo = $null
if ($VideoMetricsPath -and (Test-Path -LiteralPath $VideoMetricsPath)) {
    $rows = @(Import-Csv -LiteralPath $VideoMetricsPath)
    $sourcePts = @($rows | ForEach-Object { [long]$_.common_presentation_us })
    $sourcePackets = @($sourcePts | ForEach-Object {
        [pscustomobject]@{ pts = $_; dts = $_; duration = [long][Math]::Round(1000000.0 / $RequestedFps) }
    })
    $sourceStream = [pscustomobject]@{
        codec_name = $videoStream.codec_name; profile = $videoStream.profile
        width = $videoStream.width; height = $videoStream.height
        r_frame_rate = $videoStream.r_frame_rate; avg_frame_rate = $videoStream.avg_frame_rate
        time_base = '1/1000000'
    }
    $sourceTiming = Get-VideoTiming $sourcePackets $sourceStream $RequestedFps
    $sourceVideo = [ordered]@{
        metricsPath = [IO.Path]::GetFullPath($VideoMetricsPath)
        frameCount = $sourceTiming.frameCount
        firstCommonPresentationUs = if ($sourcePts.Count) { $sourcePts[0] } else { $null }
        lastCommonPresentationUs = if ($sourcePts.Count) { $sourcePts[-1] } else { $null }
        intervalMs = $sourceTiming.intervalMs
        duplicatePts = $sourceTiming.duplicatePts
        nonMonotonicPts = $sourceTiming.nonMonotonicPts
        missingFrameSlots = $sourceTiming.missingFrameSlots
        timingErrorAtOneMinuteMs = $sourceTiming.timingErrorAtOneMinuteMs
        timingErrorAtFiveMinutesMs = $sourceTiming.timingErrorAtFiveMinutesMs
        timingErrorSlopeMsPerMinute = $sourceTiming.timingErrorSlopeMsPerMinute
        maximumIdealSlotDeviationMs = $sourceTiming.maximumIdealSlotDeviationMs
        queueDrops = if ($rows.Count) { [long]$rows[-1].cumulative_drops } else { 0 }
    }
}

$audioCapture = $null
if ($AudioMetricsPath -and (Test-Path -LiteralPath $AudioMetricsPath)) {
    $rows = @(Import-Csv -LiteralPath $AudioMetricsPath)
    $firstDrift = if ($rows.Count) { [double]$rows[0].clock_drift_ms } else { 0.0 }
    $lastDrift = if ($rows.Count) { [double]$rows[-1].clock_drift_ms } else { 0.0 }
    $minutes = if ($rows.Count -gt 1) {
        ([long]$rows[-1].common_presentation_us - [long]$rows[0].common_presentation_us) / 60000000.0
    } else { 0.0 }
    $audioCapture = [ordered]@{
        metricsPath = [IO.Path]::GetFullPath($AudioMetricsPath)
        captureChunks = $rows.Count
        firstClockDriftMs = $firstDrift
        lastClockDriftMs = $lastDrift
        clockDriftChangeMs = [Math]::Round($lastDrift - $firstDrift, 6)
        clockDriftSlopeMsPerMinute = if ($minutes -gt 0) { [Math]::Round(($lastDrift - $firstDrift) / $minutes, 6) } else { $null }
        discontinuities = @($rows | Where-Object discontinuity -eq '1').Count
        timestampErrors = if ($rows.Count) { [long]$rows[-1].cumulative_timestamp_errors } else { 0 }
        overruns = if ($rows.Count) { [long]$rows[-1].cumulative_overruns } else { 0 }
        underruns = if ($rows.Count) { [long]$rows[-1].cumulative_underruns } else { 0 }
        queueDrops = if ($rows.Count) { [long]$rows[-1].cumulative_drops } else { 0 }
    }
}

$av = $null
if ($audio -and $videoPackets.Count -and $audioPackets.Count) {
    $videoFirst = [double]$videoStream.start_time
    $videoEnd = $videoFirst + [double]$videoStream.duration
    $audioFirst = [double]$audioStream.start_time
    $audioEnd = $audioFirst + [double]$audioStream.duration
    $initial = ($audioFirst - $videoFirst) * 1000
    $final = ($audioEnd - $videoEnd) * 1000
    $clockDriftSlope = if ($sourceVideo -and $audioCapture) {
        -$audioCapture.clockDriftSlopeMsPerMinute
    } else { $null }
    $av = [ordered]@{
        initialAudioMinusVideoStartMs = [Math]::Round($initial, 6)
        finalAudioMinusVideoEndMs = [Math]::Round($final, 6)
        coverageDifferenceChangeMs = [Math]::Round($final - $initial, 6)
        coverageDifferenceIsNotClockDrift = $true
        estimatedClockDriftMsPerMinute = if ($null -ne $clockDriftSlope) { [Math]::Round($clockDriftSlope, 6) } else { $null }
        clockDriftMethod = 'negative WASAPI sample-clock error slope; video PTS already uses Camera2 capture time in the shared Windows monotonic domain'
        videoEndSeconds = [Math]::Round($videoEnd, 6)
        audioEndSeconds = [Math]::Round($audioEnd, 6)
    }
}

$report = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    recording = $recording
    format = [ordered]@{
        name = $metadata.format.format_name
        durationSeconds = if ($metadata.format.duration) { [double]$metadata.format.duration } else { $null }
        sizeBytes = [long]$metadata.format.size
        bitrate = if ($metadata.format.bit_rate) { [long]$metadata.format.bit_rate } else { $null }
    }
    video = $video
    sourceVideo = $sourceVideo
    audio = $audio
    audioCapture = $audioCapture
    av = $av
}

$output = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $output -Encoding UTF8
Write-Output $output
$report
