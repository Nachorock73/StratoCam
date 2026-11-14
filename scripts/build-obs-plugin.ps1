[CmdletBinding()]
param(
    [string]$ObsSource,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ObsSource) { $ObsSource = Join-Path $projectRoot '.tools\obs-studio-32.2.2' }
$moduleHeader = Join-Path $ObsSource 'libobs\obs-module.h'
$toolchain = Join-Path $projectRoot '.tools\winlibs\mingw64\bin'
$cmake = Join-Path $toolchain 'cmake.exe'
$make = Join-Path $toolchain 'mingw32-make.exe'
$compiler = Join-Path $toolchain 'g++.exe'
$dlltool = Join-Path $toolchain 'dlltool.exe'
$buildDir = Join-Path $projectRoot '.tools\obs-plugin-build'
$definition = Join-Path $projectRoot 'windows\obs-plugin\obs-32.2.2.def'

foreach ($required in $moduleHeader, $definition, $cmake, $make, $compiler, $dlltool) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required OBS build input not found: $required" }
}
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    $resolved = [IO.Path]::GetFullPath($buildDir)
    $expectedParent = [IO.Path]::GetFullPath((Join-Path $projectRoot '.tools'))
    if (-not $resolved.StartsWith($expectedParent + [IO.Path]::DirectorySeparatorChar,
                                  [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected path $resolved"
    }
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$oldNativePreference = $PSNativeCommandUseErrorActionPreference
$PSNativeCommandUseErrorActionPreference = $false
try {
    & $dlltool -d $definition -D obs.dll -l (Join-Path $buildDir 'libobs.dll.a')
    if ($LASTEXITCODE -ne 0) { throw "dlltool failed with exit code $LASTEXITCODE" }
} finally { $PSNativeCommandUseErrorActionPreference = $oldNativePreference }

& $cmake -S (Join-Path $projectRoot 'windows\obs-plugin') -B $buildDir -G 'MinGW Makefiles' `
    "-DCMAKE_MAKE_PROGRAM=$make" "-DCMAKE_CXX_COMPILER=$compiler" `
    "-DOBS_SOURCE_DIR=$ObsSource" "-DOBS_IMPORT_LIBRARY=$(Join-Path $buildDir 'libobs.dll.a')"
if ($LASTEXITCODE -ne 0) { throw "OBS plugin configure failed with exit code $LASTEXITCODE" }
& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "OBS plugin build failed with exit code $LASTEXITCODE" }

$plugin = Join-Path $buildDir 'stratocam-obs.dll'
if (-not (Test-Path -LiteralPath $plugin)) { throw "OBS plugin output was not created: $plugin" }
Get-Item $plugin
