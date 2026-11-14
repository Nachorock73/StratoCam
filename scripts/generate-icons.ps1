[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $root 'logo.png'
if (-not (Test-Path -LiteralPath $sourcePath)) { throw "Canonical logo not found: $sourcePath" }

$source = [Drawing.Image]::FromFile($sourcePath)
try {
    function New-ScaledBitmap([int]$Size, [double]$ContentScale = 1.0) {
        $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::Black)
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
            $contentSize = [int][Math]::Round($Size * $ContentScale)
            $inset = [int][Math]::Round(($Size - $contentSize) / 2.0)
            $graphics.DrawImage($source, [Drawing.Rectangle]::new($inset, $inset, $contentSize, $contentSize))
        } finally { $graphics.Dispose() }
        return $bitmap
    }

    $densities = [ordered]@{ mdpi=48; hdpi=72; xhdpi=96; xxhdpi=144; xxxhdpi=192 }
    foreach ($entry in $densities.GetEnumerator()) {
        $directory = Join-Path $root "app\src\main\res\mipmap-$($entry.Key)"
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        # Leave breathing room inside both legacy and adaptive launcher masks.
        $bitmap = New-ScaledBitmap $entry.Value 0.85
        try {
            $bitmap.Save((Join-Path $directory 'ic_launcher.png'), [Drawing.Imaging.ImageFormat]::Png)
            $bitmap.Save((Join-Path $directory 'ic_launcher_round.png'), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $bitmap.Dispose() }

        $foreground = New-ScaledBitmap ([int]($entry.Value * 2.25)) 0.85
        try { $foreground.Save((Join-Path $directory 'ic_launcher_foreground.png'), [Drawing.Imaging.ImageFormat]::Png) }
        finally { $foreground.Dispose() }
    }

    $iconDirectory = Join-Path $root 'windows\resources'
    [IO.Directory]::CreateDirectory($iconDirectory) | Out-Null
    $iconPath = Join-Path $iconDirectory 'stratocam.ico'
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $images = foreach ($size in $sizes) {
        $bitmap = New-ScaledBitmap $size
        $memory = [IO.MemoryStream]::new()
        try {
            $bitmap.Save($memory, [Drawing.Imaging.ImageFormat]::Png)
            ,$memory.ToArray()
        } finally {
            $memory.Dispose()
            $bitmap.Dispose()
        }
    }
    $stream = [IO.File]::Open($iconPath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$sizes.Count)
        $offset = 6 + 16 * $sizes.Count
        for ($index = 0; $index -lt $sizes.Count; ++$index) {
            $size = $sizes[$index]
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$images[$index].Length)
            $writer.Write([uint32]$offset)
            $offset += $images[$index].Length
        }
        foreach ($image in $images) { $writer.Write($image) }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
    Write-Host "Generated Android launcher icons and $iconPath"
} finally { $source.Dispose() }
