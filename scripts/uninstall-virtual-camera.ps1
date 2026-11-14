[CmdletBinding()]
param([switch]$ElevatedPhase)

$ErrorActionPreference = 'Stop'
$programFiles64 = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $ElevatedPhase) {
    $escapedPath = $PSCommandPath.Replace("'", "''")
    $command = "& '$escapedPath' -ElevatedPhase"
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encodedCommand) -Wait -PassThru
    exit $process.ExitCode
}

$installDirectory = [IO.Path]::GetFullPath((Join-Path $programFiles64 'StratoCam\VirtualCamera'))
$dataDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'AndroidCam'))
$manager = Join-Path $installDirectory 'AndroidCamVirtualCameraManager.exe'
if (Test-Path -LiteralPath $manager) {
    foreach ($command in @('uninstall-legacy', 'uninstall-previous-current-user', 'uninstall-previous', 'uninstall-current-user', 'uninstall')) {
        & $manager $command
        if ($LASTEXITCODE -ne 0) { throw "Could not run virtual-camera removal command $command (exit $LASTEXITCODE)." }
    }
}
Remove-Item -LiteralPath 'HKLM:\SOFTWARE\Classes\CLSID\{82575855-8D10-4908-9176-1B7E5B7B0C8F}' -Recurse -Force -ErrorAction SilentlyContinue

$expectedInstall = [IO.Path]::GetFullPath("$programFiles64\StratoCam\VirtualCamera")
$expectedData = [IO.Path]::GetFullPath("$env:ProgramData\AndroidCam")
if ($installDirectory -ne $expectedInstall -or $dataDirectory -ne $expectedData) { throw 'Refusing to remove unexpected paths.' }
$frameServer = Get-Service -Name FrameServer -ErrorAction SilentlyContinue
$restartFrameServer = $frameServer -and $frameServer.Status -eq [ServiceProcess.ServiceControllerStatus]::Running
try {
    if ($restartFrameServer) { Stop-Service -Name FrameServer -Force -ErrorAction Stop }
    if (Test-Path -LiteralPath $installDirectory) { Remove-Item -LiteralPath $installDirectory -Recurse -Force }
    if (Test-Path -LiteralPath $dataDirectory) { Remove-Item -LiteralPath $dataDirectory -Recurse -Force }
} finally {
    if ($restartFrameServer) { Start-Service -Name FrameServer -ErrorAction Stop }
}
Write-Host 'StratoCam Virtual Camera completely removed.'
