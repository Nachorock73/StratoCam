[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$application = Join-Path $projectRoot 'windows\build\bin\StratoCam.exe'
if (-not (Test-Path -LiteralPath $application)) {
    & (Join-Path $PSScriptRoot 'build-windows-app.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Start-Process -FilePath $application -WorkingDirectory $projectRoot
