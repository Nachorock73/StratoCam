[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Install','Uninstall')] [string]$Action,
    [Parameter(Mandatory)] [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'
$installRootPath = [IO.Path]::GetFullPath($InstallRoot)
$manager = Join-Path $installRootPath 'VirtualCamera\AndroidCamVirtualCameraManager.exe'
$dll = Join-Path $installRootPath 'VirtualCamera\AndroidCamVirtualCamera-v3.dll'
$dataDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'AndroidCam'))
$expectedData = [IO.Path]::GetFullPath("$env:ProgramData\AndroidCam")
$frameFile = Join-Path $dataDirectory 'frames-v3.bin'
$clsid = 'HKLM:\SOFTWARE\Classes\CLSID\{82575855-8D10-4908-9176-1B7E5B7B0C8F}'

if ($Action -eq 'Install') {
    if (-not (Test-Path -LiteralPath $manager) -or -not (Test-Path -LiteralPath $dll)) {
        throw 'StratoCam Virtual Camera binaries are missing from the installation.'
    }
    $frameServer = Get-Service -Name FrameServer -ErrorAction SilentlyContinue
    $restart = $frameServer -and $frameServer.Status -eq [ServiceProcess.ServiceControllerStatus]::Running
    try {
        if ($restart) { Stop-Service -Name FrameServer -Force }
        [IO.Directory]::CreateDirectory($dataDirectory) | Out-Null
        $stream = [IO.File]::Open($frameFile, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                                  [IO.FileShare]::ReadWrite)
        try { $stream.SetLength(40MB) } finally { $stream.Dispose() }

        $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        & icacls.exe $dataDirectory '/inheritance:r' "/grant:r" "*$currentSid`:(OI)(CI)M" `
            '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)RX' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not secure the virtual-camera data directory.' }
        & icacls.exe $frameFile '/inheritance:r' "/grant:r" "*$currentSid`:M" `
            '*S-1-5-18:F' '*S-1-5-32-544:F' '*S-1-5-19:M' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not secure the virtual-camera frame file.' }

        New-Item -Path $clsid -Force | Out-Null
        Set-Item -Path $clsid -Value 'StratoCam Virtual Camera Media Source'
        $server = New-Item -Path (Join-Path $clsid 'InProcServer32') -Force
        Set-Item -Path $server.PSPath -Value $dll
        New-ItemProperty -Path $server.PSPath -Name ThreadingModel -Value Both -PropertyType String -Force | Out-Null
        # The persistent camera is machine-wide because this Windows build
        # rejects normal-consumer activation of a system-lifetime CurrentUser
        # camera created by the elevated installer. Remove pre-1.0 CurrentUser
        # registrations before creating the single AllUsers instance.
        & $manager uninstall-legacy
        if ($LASTEXITCODE -ne 0) { throw "Could not remove the legacy Android Cam virtual-camera instance (exit $LASTEXITCODE)." }
        foreach ($command in @('uninstall-previous-current-user', 'uninstall-previous')) {
            & $manager $command
            if ($LASTEXITCODE -ne 0) { throw "Could not remove previous StratoCam camera name (exit $LASTEXITCODE)." }
        }
        & $manager uninstall-current-user
        if ($LASTEXITCODE -ne 0) { throw "Could not remove the CurrentUser StratoCam virtual-camera instance (exit $LASTEXITCODE)." }
        & $manager uninstall
        if ($LASTEXITCODE -ne 0) { throw "Could not remove the previous StratoCam virtual-camera instance (exit $LASTEXITCODE)." }
        & $manager install
        if ($LASTEXITCODE -ne 0) { throw "Could not create the StratoCam virtual-camera instance (exit $LASTEXITCODE)." }
    } finally {
        if ($restart) { Start-Service -Name FrameServer }
    }
    exit 0
}

if ($dataDirectory -ne $expectedData) { throw "Refusing to remove unexpected path $dataDirectory" }
$frameServer = Get-Service -Name FrameServer -ErrorAction SilentlyContinue
$restart = $frameServer -and $frameServer.Status -eq [ServiceProcess.ServiceControllerStatus]::Running
try {
    if ($restart) { Stop-Service -Name FrameServer -Force }
    if (Test-Path -LiteralPath $manager) {
        foreach ($command in @('uninstall-legacy', 'uninstall-previous-current-user', 'uninstall-previous', 'uninstall-current-user', 'uninstall')) {
            & $manager $command
            if ($LASTEXITCODE -ne 0) { Write-Warning "Virtual-camera manager returned $LASTEXITCODE for $command." }
        }
    }
    Remove-Item -LiteralPath $clsid -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $dataDirectory) { Remove-Item -LiteralPath $dataDirectory -Recurse -Force }
} finally {
    if ($restart) { Start-Service -Name FrameServer }
}
