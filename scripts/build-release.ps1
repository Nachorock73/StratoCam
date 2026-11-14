[CmdletBinding()]
param(
    [switch]$SkipBootstrap,
    [switch]$AllowUnsignedAndroid,
    [switch]$AcceptAndroidSdkLicenses
)

$ErrorActionPreference = 'Stop'
if (-not $SkipBootstrap) {
    & (Join-Path $PSScriptRoot 'bootstrap-windows-deps.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $PSScriptRoot 'bootstrap-android-sdk.ps1') -AcceptAndroidSdkLicenses:$AcceptAndroidSdkLicenses
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $PSScriptRoot 'bootstrap-inno-setup.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& (Join-Path $PSScriptRoot 'generate-icons.ps1')
& (Join-Path $PSScriptRoot 'build-windows-app.ps1') -Clean
& (Join-Path $PSScriptRoot 'build-virtual-camera.ps1') -Clean
& (Join-Path $PSScriptRoot 'build-obs-plugin.ps1') -Clean
& (Join-Path $PSScriptRoot 'build-android-release.ps1')
& (Join-Path $PSScriptRoot 'package-release.ps1') -AllowUnsignedAndroid:$AllowUnsignedAndroid -CleanDist
