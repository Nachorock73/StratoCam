[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Stop','Restore')] [string]$Action,
    [Parameter(Mandatory)] [string]$StateFile
)
$ErrorActionPreference = 'Stop'
$service = Get-Service -Name FrameServer -ErrorAction SilentlyContinue
if ($Action -eq 'Stop') {
    if ($service -and $service.Status -eq [ServiceProcess.ServiceControllerStatus]::Running) {
        [IO.File]::WriteAllText($StateFile, 'restart')
        Stop-Service -Name FrameServer -Force -ErrorAction Stop
        $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(15))
    }
} elseif (Test-Path -LiteralPath $StateFile) {
    if ($service) { Start-Service -Name FrameServer -ErrorAction Stop }
    Remove-Item -LiteralPath $StateFile -Force
}
