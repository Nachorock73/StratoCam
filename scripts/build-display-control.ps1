[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$jdk = Join-Path $root '.tools\jdk\jdk-17.0.20+8'
$sdk = Join-Path $root '.tools\android-sdk'
$build = Join-Path $root 'android-shell\build'
$classes = Join-Path $build 'classes'
$dex = Join-Path $build 'dex'
[IO.Directory]::CreateDirectory($classes) | Out-Null
[IO.Directory]::CreateDirectory($dex) | Out-Null
$env:JAVA_HOME = $jdk
& (Join-Path $jdk 'bin\javac.exe') -source 8 -target 8 -Xlint:-options `
    -classpath (Join-Path $sdk 'platforms\android-35\android.jar') -d $classes `
    (Join-Path $root 'android-shell\src\com\stratocam\shell\DisplayPower.java')
if ($LASTEXITCODE -ne 0) { throw 'Display helper Java compilation failed.' }
$classFiles = @(Get-ChildItem -LiteralPath $classes -Filter '*.class' -Recurse | ForEach-Object FullName)
& (Join-Path $sdk 'build-tools\35.0.0\d8.bat') --min-api 23 --lib `
    (Join-Path $sdk 'platforms\android-35\android.jar') --output $dex @classFiles
if ($LASTEXITCODE -ne 0) { throw 'Display helper DEX compilation failed.' }
Copy-Item -LiteralPath (Join-Path $dex 'classes.dex') `
    -Destination (Join-Path $build 'StratoCamDisplayControl.dex') -Force
Write-Host 'Built StratoCam display power helper.'
