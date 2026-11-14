[CmdletBinding()]
param([switch]$NoBuild, [switch]$ElevatedPhase)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$programFiles64 = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $ElevatedPhase) {
    if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build-virtual-camera.ps1') }
    $escapedPath = $PSCommandPath.Replace("'", "''")
    $command = "& '$escapedPath' -NoBuild -ElevatedPhase"
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encodedCommand)
    $process = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { exit $process.ExitCode }
    Write-Host 'StratoCam Virtual Camera installed for all Windows users.'
    exit 0
}

if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build-virtual-camera.ps1') }
$build = Join-Path $root 'windows\virtual-camera\build\bin'
$dll = Join-Path $build 'AndroidCamVirtualCamera.dll'
$manager = Join-Path $build 'AndroidCamVirtualCameraManager.exe'
if (-not (Test-Path -LiteralPath $dll) -or -not (Test-Path -LiteralPath $manager)) {
    throw 'Virtual camera binaries are missing. Run build-virtual-camera.ps1 first.'
}

$installDirectory = Join-Path $programFiles64 'StratoCam\VirtualCamera'
$dataDirectory = Join-Path $env:ProgramData 'AndroidCam'
$frameFile = Join-Path $dataDirectory 'frames-v3.bin'
$frameServer = Get-Service -Name FrameServer -ErrorAction SilentlyContinue
$restartFrameServer = $frameServer -and $frameServer.Status -eq [ServiceProcess.ServiceControllerStatus]::Running
try {
    if ($restartFrameServer) { Stop-Service -Name FrameServer -Force -ErrorAction Stop }
    [IO.Directory]::CreateDirectory($installDirectory) | Out-Null
    [IO.Directory]::CreateDirectory($dataDirectory) | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $installDirectory 'AndroidCamVirtualCamera-v3.dll') -Force
    Copy-Item -LiteralPath $manager -Destination $installDirectory -Force

    $stream = [IO.File]::Open($frameFile, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try { $stream.SetLength(40MB) } finally { $stream.Dispose() }

    $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $dataDirectory '/inheritance:r' "/grant:r" "*$currentSid`:(OI)(CI)M" '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not secure the virtual camera data directory.' }
    & icacls.exe $frameFile '/inheritance:r' "/grant:r" "*$currentSid`:M" '*S-1-5-18:F' '*S-1-5-32-544:F' '*S-1-5-19:M' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not secure the virtual camera frame file.' }

    $registeredDll = Join-Path $installDirectory 'AndroidCamVirtualCamera-v3.dll'
    $clsid = 'HKLM:\SOFTWARE\Classes\CLSID\{82575855-8D10-4908-9176-1B7E5B7B0C8F}'
    New-Item -Path $clsid -Force | Out-Null
    Set-Item -Path $clsid -Value 'StratoCam Virtual Camera Media Source'
    $server = New-Item -Path (Join-Path $clsid 'InProcServer32') -Force
    Set-Item -Path $server.PSPath -Value $registeredDll
    New-ItemProperty -Path $server.PSPath -Name 'ThreadingModel' -Value 'Both' -PropertyType String -Force | Out-Null

    $installedManager = Join-Path $installDirectory 'AndroidCamVirtualCameraManager.exe'
    foreach ($command in @('uninstall-legacy', 'uninstall-previous-current-user', 'uninstall-previous', 'uninstall-current-user', 'uninstall')) {
        & $installedManager $command
        if ($LASTEXITCODE -ne 0) { throw "Virtual-camera migration command $command failed with exit code $LASTEXITCODE" }
    }
    & $installedManager install
    if ($LASTEXITCODE -ne 0) { throw "IMFVirtualCamera registration failed with exit code $LASTEXITCODE" }
} finally {
    if ($restartFrameServer) { Start-Service -Name FrameServer -ErrorAction Stop }
}
Write-Host 'StratoCam Virtual Camera installed for all Windows users.'
