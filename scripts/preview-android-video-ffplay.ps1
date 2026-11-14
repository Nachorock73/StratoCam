[CmdletBinding()]
param(
    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 0,

    [string]$DiagnosticCopyPath
)

$ErrorActionPreference = 'Stop'
$previewScript = Join-Path $PSScriptRoot 'preview-android-video.ps1'

& $previewScript -Decoder FFplay -DurationSeconds $DurationSeconds -DiagnosticCopyPath $DiagnosticCopyPath
