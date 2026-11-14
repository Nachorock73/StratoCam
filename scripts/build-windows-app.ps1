[CmdletBinding()]
param([switch]$Clean)

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
$toolchain = Join-Path $projectRoot '.tools\winlibs\mingw64'
$openh264 = Join-Path $projectRoot '.tools\openh264-install'
$ffmpeg = Join-Path $projectRoot '.tools\ffmpeg-dev\ffmpeg-n9.0-latest-win64-lgpl-shared-9.0'
$cmake = Join-Path $toolchain 'bin\cmake.exe'
$make = Join-Path $toolchain 'bin\mingw32-make.exe'
$compiler = Join-Path $toolchain 'bin\g++.exe'
$build = Join-Path $projectRoot 'windows\build'

if (-not (Test-Path -LiteralPath $cmake) -or
    -not (Test-Path -LiteralPath (Join-Path $openh264 'include\wels\codec_api.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $ffmpeg 'include\libavformat\avformat.h'))) {
    throw 'Windows dependencies are missing. Run .\scripts\bootstrap-windows-deps.ps1 first.'
}
if ($Clean -and (Test-Path -LiteralPath $build)) {
    $resolvedBuild = [System.IO.Path]::GetFullPath($build)
    $expectedParent = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'windows'))
    if ([System.IO.Path]::GetDirectoryName($resolvedBuild) -ne $expectedParent) { throw "Refusing to clean unexpected path $resolvedBuild" }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}
[System.IO.Directory]::CreateDirectory($build) | Out-Null
$env:Path = "$(Join-Path $toolchain 'bin');$env:Path"
Invoke-NativeChecked {
    & $cmake -S (Join-Path $projectRoot 'windows') -B $build -G 'MinGW Makefiles' `
        "-DCMAKE_MAKE_PROGRAM=$make" `
        "-DCMAKE_CXX_COMPILER=$compiler" `
        "-DOPENH264_ROOT=$openh264" `
        "-DFFMPEG_ROOT=$ffmpeg" `
        '-DCMAKE_BUILD_TYPE=Release'
}
Invoke-NativeChecked { & $cmake --build $build --config Release }
Invoke-NativeChecked { & $cmake --build $build --target test }
Write-Host "Windows application: $(Join-Path $build 'bin\StratoCam.exe')"
