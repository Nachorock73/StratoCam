[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
function Invoke-NativeChecked([scriptblock]$Command) {
    $old = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $Command; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "Native build command failed with exit code $code" }
}

$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $root '.tools\winlibs\mingw64\bin\g++.exe'
$source = Join-Path $root 'windows\virtual-camera'
$output = Join-Path $source 'build\bin'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Windows dependencies are missing. Run bootstrap-windows-deps.ps1 first.' }
if ($Clean -and (Test-Path -LiteralPath $output)) {
    $resolved = [IO.Path]::GetFullPath($output)
    $expected = [IO.Path]::GetFullPath((Join-Path $source 'build\bin'))
    if ($resolved -ne $expected) { throw "Refusing to clean unexpected path $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
[IO.Directory]::CreateDirectory($output) | Out-Null

$common = @('-std=c++20','-O2','-DUNICODE','-D_UNICODE','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00','-static')
Push-Location $root
try {
Invoke-NativeChecked {
    & $compiler @common '-Iwindows' '-Iwindows/shared' '-Iwindows/virtual-camera' '-shared' `
        'windows/virtual-camera/VirtualCameraSource.cpp' `
        'windows/virtual-camera/AndroidCamVirtualCamera.def' `
        '-o' (Join-Path $output 'AndroidCamVirtualCamera.dll') `
        '-lmfplat' '-lmf' '-lmfuuid' '-lksguid' '-luuid' '-lole32' '-lpropsys'
}
Invoke-NativeChecked {
    & $compiler @common '-municode' '-Iwindows' '-Iwindows/shared' '-Iwindows/virtual-camera' `
        'windows/virtual-camera/VirtualCameraManager.cpp' `
        '-o' (Join-Path $output 'AndroidCamVirtualCameraManager.exe') `
        '-lmfplat' '-luuid' '-lole32' '-lpropsys'
}
Invoke-NativeChecked {
    & $compiler @common '-municode' '-Iwindows/virtual-camera' `
        'windows/virtual-camera/VirtualCameraProbe.cpp' `
        '-o' (Join-Path $output 'AndroidCamVirtualCameraProbe.exe') `
        '-lmfreadwrite' '-lmfplat' '-lmf' '-lmfuuid' '-luuid' '-lole32' '-lpropsys'
}
Invoke-NativeChecked {
    & $compiler @common '-municode' '-Iwindows' '-Iwindows/shared' `
        'windows/virtual-camera/VirtualCameraMetrics.cpp' `
        '-o' (Join-Path $output 'AndroidCamVirtualCameraMetrics.exe')
}
Write-Host "Virtual camera binaries: $output"
} finally {
    Pop-Location
}
