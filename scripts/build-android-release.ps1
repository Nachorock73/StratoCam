[CmdletBinding()]
param([switch]$NoClean, [switch]$SkipTests)

$ErrorActionPreference = 'Stop'
function Invoke-NativeChecked([scriptblock]$Command) {
    $old = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $Command; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $old }
    if ($code -ne 0) { throw "Android build failed with exit code $code" }
}

$root = Split-Path -Parent $PSScriptRoot
$version = ConvertFrom-StringData (Get-Content -Raw (Join-Path $root 'version.properties'))
$javaHome = Join-Path $root '.tools\jdk\jdk-17.0.20+8'
$sdk = Join-Path $root '.tools\android-sdk'
if (-not (Test-Path -LiteralPath (Join-Path $javaHome 'bin\java.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $sdk 'platforms\android-35\android.jar'))) {
    throw 'Android build dependencies are missing. Run scripts\bootstrap-android-sdk.ps1 first.'
}
$env:JAVA_HOME = $javaHome
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_USER_HOME = Join-Path $root '.android'
$env:GRADLE_USER_HOME = Join-Path $root '.gradle'
$androidUserHome = Join-Path $root '.android-home'
[IO.Directory]::CreateDirectory($androidUserHome) | Out-Null
$env:GRADLE_OPTS = (($env:GRADLE_OPTS, "-Duser.home=$androidUserHome") -join ' ').Trim()
$env:JAVA_TOOL_OPTIONS = (($env:JAVA_TOOL_OPTIONS, "-Duser.home=$androidUserHome") -join ' ').Trim()

$arguments = @('--no-daemon')
if (-not $NoClean) { $arguments += 'clean' }
if (-not $SkipTests) { $arguments += 'testReleaseUnitTest' }
$arguments += 'assembleRelease'

$signingFile = Join-Path $root '.signing\release-signing.properties'
if (Test-Path -LiteralPath $signingFile) {
    $signing = ConvertFrom-StringData (Get-Content -Raw $signingFile)
    foreach ($name in 'stratocam.keystore','stratocam.storePassword','stratocam.keyAlias','stratocam.keyPassword') {
        if (-not $signing[$name]) { throw "$signingFile is missing $name" }
    }
    # Environment variables keep passwords out of the Gradle process command
    # line while preserving the same provider-based configuration in Gradle.
    $env:STRATOCAM_KEYSTORE = $signing['stratocam.keystore']
    $env:STRATOCAM_KEYSTORE_PASSWORD = $signing['stratocam.storePassword']
    $env:STRATOCAM_KEY_ALIAS = $signing['stratocam.keyAlias']
    $env:STRATOCAM_KEY_PASSWORD = $signing['stratocam.keyPassword']
}
$signingEnvironment = @(
    $env:STRATOCAM_KEYSTORE,
    $env:STRATOCAM_KEYSTORE_PASSWORD,
    $env:STRATOCAM_KEY_ALIAS,
    $env:STRATOCAM_KEY_PASSWORD
)
if (($signingEnvironment | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -notin 0, 4) {
    throw 'Android release signing is only partially configured. Supply all four STRATOCAM signing values.'
}
Push-Location $root
try {
    Invoke-NativeChecked { & (Join-Path $root 'gradlew.bat') @arguments }
} finally {
    Pop-Location
}

$signed = Join-Path $root 'app\build\outputs\apk\release\app-release.apk'
$unsigned = Join-Path $root 'app\build\outputs\apk\release\app-release-unsigned.apk'
$dist = Join-Path $root 'dist'
[IO.Directory]::CreateDirectory($dist) | Out-Null
$apksigner = Join-Path $sdk 'build-tools\35.0.0\apksigner.bat'
if (Test-Path -LiteralPath $signed) {
    Invoke-NativeChecked { & $apksigner verify --verbose --print-certs $signed }
    $destination = Join-Path $dist "StratoCam-Android-v$($version.versionName).apk"
    Copy-Item -LiteralPath $signed -Destination $destination -Force
    Write-Host "Signed Android release: $destination"
} elseif (Test-Path -LiteralPath $unsigned) {
    $destination = Join-Path $dist "StratoCam-Android-v$($version.versionName)-unsigned.apk"
    Copy-Item -LiteralPath $unsigned -Destination $destination -Force
    Write-Warning "No release signing configuration was supplied. Unsigned artifact: $destination"
} else {
    throw 'Gradle did not produce a release APK.'
}
Get-Item -LiteralPath $destination
