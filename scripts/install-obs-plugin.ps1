[CmdletBinding()]
param([string]$PluginPath)

$ErrorActionPreference = 'Stop'
if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw 'Close OBS Studio before installing or updating the StratoCam plugin.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $PluginPath) { $PluginPath = Join-Path $projectRoot '.tools\obs-plugin-build\stratocam-obs.dll' }
if (-not (Test-Path -LiteralPath $PluginPath)) {
    throw "Plugin not found at $PluginPath. Run scripts\build-obs-plugin.ps1 first."
}
$pluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins\stratocam-obs'
$destination = Join-Path $pluginRoot 'bin\64bit'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $destination 'stratocam-obs.dll') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'windows\obs-plugin\LICENSE_NOTICE.md') `
    -Destination (Join-Path $pluginRoot 'LICENSE_NOTICE.md') -Force
Get-Item (Join-Path $destination 'stratocam-obs.dll')
