[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
function Invoke-NativeChecked([scriptblock]$Command) {
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Command
        $nativeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($nativeExitCode -ne 0) { exit $nativeExitCode }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $projectRoot '.tools'
$downloads = Join-Path $tools 'windows-deps-download'
[System.IO.Directory]::CreateDirectory($downloads) | Out-Null

$dependencies = @(
    @{
        Name = 'WinLibs GCC 16.2 MinGW-w64 UCRT'
        Uri = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-14.0.0-ucrt-r1/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip'
        Archive = 'winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r1.zip'
        Sha256 = 'c1f52294597c0b73786b2a78eb5d176d89226d2f21875eab75e783a8b1cefcc4'
        Destination = 'winlibs'
        Probe = 'winlibs\mingw64\bin\g++.exe'
    },
    @{
        Name = 'FFmpeg 9 LGPL shared development build'
        Uri = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip'
        Archive = 'ffmpeg-n9.0.1-11-ge47273f4d9-20260830-win64-lgpl-shared.zip'
        Sha256 = '66e7e9a7f9db2b492145e6fd045e5e756f0eedb267b521f111dd22339a4c6dc2'
        Destination = 'ffmpeg-dev'
        Probe = 'ffmpeg-dev\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0\include\libavformat\avformat.h'
    }
)

foreach ($dependency in $dependencies) {
    $probe = Join-Path $tools $dependency.Probe
    if (Test-Path -LiteralPath $probe) {
        Write-Host "$($dependency.Name) already available."
        continue
    }
    $archive = Join-Path $downloads $dependency.Archive
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $($dependency.Name)..."
        Invoke-WebRequest -Uri $dependency.Uri -OutFile $archive
    }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    if ($actualHash -ne $dependency.Sha256) {
        throw "Checksum mismatch for $($dependency.Name): expected $($dependency.Sha256), got $actualHash"
    }
    $destination = Join-Path $tools $dependency.Destination
    [System.IO.Directory]::CreateDirectory($destination) | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
    if (-not (Test-Path -LiteralPath $probe)) { throw "$($dependency.Name) extraction did not produce $probe" }
}

$toolchain = Join-Path $tools 'winlibs\mingw64'
$openh264Source = Join-Path $tools 'openh264-source'
$openh264Install = Join-Path $tools 'openh264-install'
$openh264Probe = Join-Path $openh264Install 'include\wels\codec_api.h'
$openh264Commit = '652bdb7719f30b52b08e506645a7322ff1b2cc6f'
if (-not (Test-Path -LiteralPath $openh264Probe)) {
    if (-not (Test-Path -LiteralPath (Join-Path $openh264Source '.git'))) {
        Invoke-NativeChecked { & git clone --filter=blob:none --no-checkout https://github.com/cisco/openh264.git $openh264Source }
    }
    Invoke-NativeChecked { & git -C $openh264Source fetch --depth 1 origin $openh264Commit }
    Invoke-NativeChecked { & git -C $openh264Source checkout --detach $openh264Commit }

    $mesonRoot = Join-Path $tools 'meson'
    $mesonModule = Join-Path $mesonRoot 'mesonbuild\mesonmain.py'
    if (-not (Test-Path -LiteralPath $mesonModule)) {
        Invoke-NativeChecked { & py -m pip install --disable-pip-version-check --no-warn-script-location --target $mesonRoot 'meson==1.12.0' }
    }
    if (-not (Test-Path -LiteralPath $mesonModule)) { throw 'Meson installation did not produce mesonbuild/mesonmain.py.' }
    $env:PYTHONPATH = (($mesonRoot, $env:PYTHONPATH) -join ';').TrimEnd(';')
    $env:Path = "$(Join-Path $toolchain 'bin');$env:Path"
    $build = Join-Path $tools 'openh264-build'
    $crossFile = Join-Path $projectRoot 'windows\toolchains\openh264-mingw64.ini'
    Invoke-NativeChecked { & py -m mesonbuild.mesonmain setup $build $openh264Source --cross-file $crossFile --default-library static --buildtype release -Dtests=disabled --prefix $openh264Install }
    Invoke-NativeChecked { & py -m mesonbuild.mesonmain compile -C $build }
    Invoke-NativeChecked { & py -m mesonbuild.mesonmain install -C $build }
}
Write-Host 'OpenH264 2.6.0 static decoder is available.'

$obsSource = Join-Path $tools 'obs-studio-32.2.2'
$obsCommit = 'ba2f32bdf791005443988a4955e963663e16b1ed'
if (-not (Test-Path -LiteralPath (Join-Path $obsSource 'libobs\obs-module.h'))) {
    if (-not (Test-Path -LiteralPath (Join-Path $obsSource '.git'))) {
        Invoke-NativeChecked { & git clone --filter=blob:none --no-checkout https://github.com/obsproject/obs-studio.git $obsSource }
    }
    Invoke-NativeChecked { & git -C $obsSource fetch --depth 1 origin $obsCommit }
    Invoke-NativeChecked { & git -C $obsSource checkout --detach $obsCommit }
}
Write-Host 'OBS Studio 32.2.2 headers are available.'
