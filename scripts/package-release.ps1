[CmdletBinding()]
param([switch]$AllowUnsignedAndroid, [switch]$CleanDist)

$ErrorActionPreference = 'Stop'
function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { throw "Required release input is missing: $Source" }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Destination)) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}
function Assert-ChildPath([string]$Path, [string]$Parent) {
    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $resolvedParent = [IO.Path]::GetFullPath($Parent)
    if (-not $resolvedPath.StartsWith($resolvedParent + [IO.Path]::DirectorySeparatorChar,
                                      [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify unexpected path $resolvedPath"
    }
}

$root = Split-Path -Parent $PSScriptRoot
$release = ConvertFrom-StringData (Get-Content -Raw (Join-Path $root 'version.properties'))
$version = $release.versionName
$stagingParent = Join-Path $root 'release\staging'
$stage = Join-Path $stagingParent 'StratoCam'
$dist = Join-Path $root 'dist'
$signedApkInput = Join-Path $root 'app\build\outputs\apk\release\app-release.apk'
$unsignedApkInput = Join-Path $root 'app\build\outputs\apk\release\app-release-unsigned.apk'
Assert-ChildPath $stage $root
Assert-ChildPath $dist $root
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if ($CleanDist -and (Test-Path -LiteralPath $dist)) { Remove-Item -LiteralPath $dist -Recurse -Force }
[IO.Directory]::CreateDirectory($stage) | Out-Null
[IO.Directory]::CreateDirectory($dist) | Out-Null

$windowsBin = Join-Path $root 'windows\build\bin'
Copy-Required (Join-Path $windowsBin 'StratoCam.exe') (Join-Path $stage 'StratoCam.exe')
foreach ($file in 'avformat-63.dll','avcodec-63.dll','avutil-61.dll','swresample-7.dll','libwinpthread-1.dll') {
    Copy-Required (Join-Path $windowsBin $file) (Join-Path $stage $file)
}

$platformTools = Join-Path $root '.tools\android-sdk\platform-tools'
foreach ($file in 'adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll') {
    Copy-Required (Join-Path $platformTools $file) (Join-Path $stage "platform-tools\$file")
}

$virtual = Join-Path $root 'windows\virtual-camera\build\bin'
Copy-Required (Join-Path $virtual 'AndroidCamVirtualCamera.dll') `
    (Join-Path $stage 'VirtualCamera\AndroidCamVirtualCamera-v3.dll')
Copy-Required (Join-Path $virtual 'AndroidCamVirtualCameraManager.exe') `
    (Join-Path $stage 'VirtualCamera\AndroidCamVirtualCameraManager.exe')
Copy-Required (Join-Path $virtual 'AndroidCamVirtualCameraProbe.exe') `
    (Join-Path $stage 'VirtualCamera\AndroidCamVirtualCameraProbe.exe')
Copy-Required (Join-Path $root 'installer\virtual-camera-registration.ps1') `
    (Join-Path $stage 'Support\virtual-camera-registration.ps1')

$obsPlugin = Join-Path $root '.tools\obs-plugin-build\stratocam-obs.dll'
Copy-Required $obsPlugin (Join-Path $stage 'OBS\stratocam-obs.dll')
Copy-Required (Join-Path $root 'windows\obs-plugin\LICENSE_NOTICE.md') `
    (Join-Path $stage 'OBS\LICENSE_NOTICE.md')

$signedApk = Join-Path $dist "StratoCam-Android-v$version.apk"
$unsignedApk = Join-Path $dist "StratoCam-Android-v$version-unsigned.apk"
if (Test-Path -LiteralPath $signedApkInput) {
    Copy-Required $signedApkInput $signedApk
    $apk = $signedApk
    $setupBase = "StratoCam-Setup-v$version"
} elseif ($AllowUnsignedAndroid -and (Test-Path -LiteralPath $unsignedApkInput)) {
    Copy-Required $unsignedApkInput $unsignedApk
    $apk = $unsignedApk
    $setupBase = "StratoCam-Setup-v$version-unsigned-android"
} else {
    throw "Signed Android artifact is missing: $signedApk. Configure release signing first."
}
Copy-Required $apk (Join-Path $stage "Android\$([IO.Path]::GetFileName($apk))")

Copy-Required (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Required (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.md')
Copy-Required (Join-Path $root '.tools\ffmpeg-dev\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\LICENSE.txt') `
    (Join-Path $stage 'licenses\FFMPEG-LICENSE.txt')
Copy-Required (Join-Path $root '.tools\openh264-source\LICENSE') `
    (Join-Path $stage 'licenses\OpenH264-LICENSE.txt')
Copy-Required (Join-Path $platformTools 'NOTICE.txt') `
    (Join-Path $stage 'licenses\Android-platform-tools-NOTICE.txt')
Copy-Required (Join-Path $root 'third_party\licenses\Opus-COPYING.txt') `
    (Join-Path $stage 'licenses\Opus-COPYING.txt')
Copy-Required (Join-Path $root '.tools\obs-studio-32.2.2\COPYING') `
    (Join-Path $stage 'licenses\OBS-GPL-2.0.txt')

$compiler = Join-Path $root '.tools\inno-setup-7\ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Run scripts\bootstrap-inno-setup.ps1 first.' }
$installerScript = Join-Path $root 'installer\StratoCam.iss'
& $compiler "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$dist" `
    "/DSetupBaseName=$setupBase" $installerScript
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
$installer = Join-Path $dist "$setupBase.exe"
if (-not (Test-Path -LiteralPath $installer)) { throw "Installer was not produced: $installer" }

Copy-Required (Join-Path $root 'LICENSE') (Join-Path $dist 'LICENSE.txt')
Copy-Required (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $dist 'THIRD_PARTY_NOTICES.md')
$distLicenses = Join-Path $dist 'licenses'
if (Test-Path -LiteralPath $distLicenses) { Remove-Item -LiteralPath $distLicenses -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $stage 'licenses') -Destination $distLicenses -Recurse

$checksumPath = Join-Path $dist 'SHA256SUMS.txt'
$files = Get-ChildItem -LiteralPath $dist -Recurse -File | Where-Object FullName -ne $checksumPath | Sort-Object FullName
$lines = foreach ($file in $files) {
    $relative = [IO.Path]::GetRelativePath($dist, $file.FullName).Replace('\','/')
    "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant(), $relative
}
[IO.File]::WriteAllLines($checksumPath, $lines, [Text.UTF8Encoding]::new($false))
Write-Host "Release installer: $installer"
Write-Host "Checksums: $checksumPath"
