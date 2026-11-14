[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$download = Join-Path $root '.tools\windows-deps-download\innosetup-7.1.0-x64.exe'
$install = Join-Path $root '.tools\inno-setup-7'
$compiler = Join-Path $install 'ISCC.exe'
if (Test-Path -LiteralPath $compiler) { Write-Host "Inno Setup: $compiler"; exit 0 }
[IO.Directory]::CreateDirectory((Split-Path -Parent $download)) | Out-Null
if (-not (Test-Path -LiteralPath $download)) {
    Invoke-WebRequest -Uri 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe' `
        -OutFile $download
}
$expected = '0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f'
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $download).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "Inno Setup checksum mismatch: expected $expected, got $actual" }
$signature = Get-AuthenticodeSignature -LiteralPath $download
if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
    $signature.SignerCertificate.Subject -notmatch 'CN=Pyrsys B\.V\.') {
    throw "Inno Setup Authenticode verification failed: $($signature.Status)"
}
$process = Start-Process -FilePath $download -ArgumentList @(
    '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',"/DIR=$install") -Wait -PassThru
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler)) {
    throw "Inno Setup installation failed with exit code $($process.ExitCode)"
}
Write-Host "Inno Setup: $compiler"
