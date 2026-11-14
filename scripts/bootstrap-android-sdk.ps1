[CmdletBinding()]
param([switch]$AcceptAndroidSdkLicenses)

$ErrorActionPreference = 'Stop'
function Invoke-NativeChecked([scriptblock]$Command) {
    $old = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $Command; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "Native command failed with exit code $code" }
}
function Get-VerifiedDownload([string]$Uri, [string]$Path, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Path)) { Invoke-WebRequest -Uri $Uri -OutFile $Path }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "Checksum mismatch for $Path`: expected $Sha256, got $actual" }
}

$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root '.tools'
$downloads = Join-Path $tools 'android-deps-download'
$sdk = Join-Path $tools 'android-sdk'
[IO.Directory]::CreateDirectory($downloads) | Out-Null
[IO.Directory]::CreateDirectory($sdk) | Out-Null

$jdkArchive = Join-Path $downloads 'OpenJDK17U-jdk_x64_windows_hotspot_17.0.20_8.zip'
Get-VerifiedDownload `
    'https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.20%2B8/OpenJDK17U-jdk_x64_windows_hotspot_17.0.20_8.zip' `
    $jdkArchive '418497be5cf585bdd2203d6486a565d66d3f5e992d5630d45104cb873fab8122'
$jdk = Join-Path $tools 'jdk\jdk-17.0.20+8'
if (-not (Test-Path -LiteralPath (Join-Path $jdk 'bin\java.exe'))) {
    [IO.Directory]::CreateDirectory((Join-Path $tools 'jdk')) | Out-Null
    Expand-Archive -LiteralPath $jdkArchive -DestinationPath (Join-Path $tools 'jdk') -Force
}

$commandLineArchive = Join-Path $downloads 'commandlinetools-win-13114758_latest.zip'
Get-VerifiedDownload 'https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip' `
    $commandLineArchive '98b565cb657b012dae6794cefc0f66ae1efb4690c699b78a614b4a6a3505b003'
$commandLineRoot = Join-Path $sdk 'cmdline-tools\latest'
$sdkManager = Join-Path $commandLineRoot 'bin\sdkmanager.bat'
if (-not (Test-Path -LiteralPath $sdkManager)) {
    $temporary = Join-Path $tools 'android-commandline-extract'
    $resolvedTemporary = [IO.Path]::GetFullPath($temporary)
    $resolvedTools = [IO.Path]::GetFullPath($tools)
    if (-not $resolvedTemporary.StartsWith($resolvedTools + [IO.Path]::DirectorySeparatorChar,
                                           [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to use unexpected temporary path $resolvedTemporary"
    }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
    Expand-Archive -LiteralPath $commandLineArchive -DestinationPath $temporary -Force
    [IO.Directory]::CreateDirectory((Split-Path -Parent $commandLineRoot)) | Out-Null
    Copy-Item -LiteralPath (Join-Path $temporary 'cmdline-tools') -Destination $commandLineRoot -Recurse
    Remove-Item -LiteralPath $temporary -Recurse -Force
}

$env:JAVA_HOME = $jdk
if ($AcceptAndroidSdkLicenses) {
    $answers = 1..100 | ForEach-Object { 'y' }
    $answers | & $sdkManager "--sdk_root=$sdk" --licenses | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Android SDK license acceptance failed with exit code $LASTEXITCODE" }
}
Invoke-NativeChecked { & $sdkManager "--sdk_root=$sdk" 'platform-tools' 'platforms;android-35' 'build-tools;35.0.0' }

$escapedSdk = $sdk.Replace('\', '/')
[IO.File]::WriteAllText((Join-Path $root 'local.properties'), "sdk.dir=$escapedSdk`r`n",
                        [Text.UTF8Encoding]::new($false))
Write-Host "Android SDK: $sdk"
Write-Host "JDK: $jdk"
