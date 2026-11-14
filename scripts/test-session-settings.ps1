[CmdletBinding()]
param([ValidateSet(0,1)] [int]$VideoMode = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'windows\build\bin\StratoCam.exe'
$keyPath = 'Software\AndroidCamStudio'
$key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
$originalPreference = $key.GetValue('SessionSettings', $null)
$process = $null
$control = Join-Path $PSScriptRoot 'android-cam-control.ps1'
$log = Join-Path $root 'captures\windows-pipeline-diagnostics.log'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SessionTestWindows {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr value);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr hwnd, string value);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder value, int size);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendText(IntPtr hwnd, uint msg, IntPtr w, string value);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr ReadText(IntPtr hwnd, uint msg, IntPtr w, StringBuilder value);
    public static IntPtr Find(uint target) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((hwnd, v) => { uint pid; GetWindowThreadProcessId(hwnd, out pid);
            if (pid==target && GetDlgItem(hwnd,100)!=IntPtr.Zero) { result=hwnd; return false; } return true; },IntPtr.Zero);
        return result;
    }
}
'@

function Start-TestApp {
    $started = [DateTime]::UtcNow
    $script:process = Start-Process -FilePath $exe -WorkingDirectory $root -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        Start-Sleep -Milliseconds 200
        if ($process.HasExited) { throw 'Application exited during preference test.' }
        $text = if ((Test-Path $log) -and (Get-Item $log).LastWriteTimeUtc -ge $started) { Get-Content $log -Raw } else { '' }
    } while ($text -notmatch 'Live preview visible' -and [DateTime]::UtcNow -lt $deadline)
    if ($text -notmatch 'Live preview visible') { throw 'Preview did not start during preference test.' }
    $script:window = [SessionTestWindows]::Find([uint32]$process.Id)
    if ($window -eq [IntPtr]::Zero) { throw 'Test application window not found.' }
}

function Close-TestApp {
    if ($process -and -not $process.HasExited) {
        [void][SessionTestWindows]::PostMessage([SessionTestWindows]::Find([uint32]$process.Id),0x0010,[IntPtr]::Zero,[IntPtr]::Zero)
        if (-not $process.WaitForExit(25000)) { throw 'Application did not close during preference test.' }
    }
}

function Select-Combo([int]$Id, [int]$Selection) {
    $combo = [SessionTestWindows]::GetDlgItem($window,$Id)
    [void][SessionTestWindows]::SendMessage($combo,0x014E,[IntPtr]$Selection,[IntPtr]::Zero)
    [void][SessionTestWindows]::SendMessage($window,0x0111,[IntPtr](65536 -bor $Id),$combo)
}

function Assert-Combo([int]$Id, [int]$Expected) {
    $actual = [SessionTestWindows]::SendMessage([SessionTestWindows]::GetDlgItem($window,$Id),0x0147,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
    if ($actual -ne $Expected) { throw "Control $Id did not restore its saved selection." }
}

$originalCamera = (& $control get-state).state
try {
    foreach ($existing in (Get-Process StratoCam -ErrorAction SilentlyContinue)) {
        if (-not $existing.CloseMainWindow() -or -not $existing.WaitForExit(25000)) { throw 'Close StratoCam before running this test.' }
    }
    $originalPreference = $key.GetValue('SessionSettings', $null)
    $seed = [ordered]@{
        schema=1; videoMode=$VideoMode; orientation=3; scale=1; audioOffsetMs=125; audioDeviceId=''; hasCameraState=$true
        autoExposure=$originalCamera.autoExposure; iso=$originalCamera.iso; exposureNs=$originalCamera.exposureTimeNs
        autoFocus=$originalCamera.autoFocus; focusDistance=$originalCamera.focusDistanceDiopters
        autoWhiteBalance=$originalCamera.autoWhiteBalance; redGain=$originalCamera.redGain
        blueGain=$originalCamera.blueGain; zoom=$originalCamera.zoomRatio
    }
    $key.SetValue('SessionSettings',($seed|ConvertTo-Json -Compress),[Microsoft.Win32.RegistryValueKind]::String)
    Start-TestApp
    Assert-Combo 121 $VideoMode
    Assert-Combo 115 3
    Assert-Combo 116 1
    Select-Combo 115 5
    Select-Combo 116 2
    [void][SessionTestWindows]::SendText([SessionTestWindows]::GetDlgItem($window,124),0x000C,[IntPtr]::Zero,'-75')
    # Exercise camera modes through the actual Windows controls, not a preference-file rewrite.
    foreach ($id in @(102,105,107,113)) {
        [void][SessionTestWindows]::SendMessage($window,0x0111,[IntPtr]$id,[IntPtr]::Zero)
    }
    $expectedCamera = (& $control get-state).state
    Close-TestApp
    $saved = $key.GetValue('SessionSettings') | ConvertFrom-Json
    [ordered]@{saved=$saved;expectedCamera=$expectedCamera} | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $root 'captures\session-settings-test.json') -Encoding UTF8
    if ($saved.orientation -ne 5 -or $saved.scale -ne 2 -or $saved.audioOffsetMs -ne -75 -or
        $saved.autoExposure -ne $expectedCamera.autoExposure -or $saved.autoFocus -ne $expectedCamera.autoFocus -or
        $saved.autoWhiteBalance -ne $expectedCamera.autoWhiteBalance -or [Math]::Abs($saved.zoom-$expectedCamera.zoomRatio) -gt 0.001) {
        throw 'Closing did not save the complete session.'
    }
    # Change phone-side state so the second launch must actually reapply the saved settings.
    & $control reset-auto | Out-Null
    Start-TestApp
    Assert-Combo 121 $VideoMode
    Assert-Combo 115 5
    Assert-Combo 116 2
    $offsetText = [Text.StringBuilder]::new(32)
    [void][SessionTestWindows]::ReadText([SessionTestWindows]::GetDlgItem($window,124),0x000D,[IntPtr]32,$offsetText)
    if ($offsetText.ToString() -ne '-75') { throw 'Audio offset did not restore.' }
    $actualCamera = (& $control get-state).state
    foreach ($name in @('autoExposure','autoFocus','autoWhiteBalance','iso','exposureTimeNs','focusDistanceDiopters','redGain','blueGain','zoomRatio')) {
        if ([Math]::Abs([double]$actualCamera.$name-[double]$expectedCamera.$name) -gt 0.001) {
            throw "Camera setting $name did not restore after relaunch."
        }
    }
    if (-not (Select-String -LiteralPath $log -Pattern 'Camera settings restored' -Quiet)) { throw 'Camera restore was not confirmed.' }
    [pscustomobject]@{Passed=$true;CloseReopen=$true;CameraControlsRestored=$true;UiSelectionsRestored=$true;AudioOffsetRestored=$true}
} finally {
    Close-TestApp
    & $control set-exposure -Mode $(if($originalCamera.autoExposure){'Auto'}else{'Manual'}) -Iso $originalCamera.iso -ExposureTimeNs $originalCamera.exposureTimeNs | Out-Null
    & $control set-focus -Mode $(if($originalCamera.autoFocus){'Auto'}else{'Manual'}) -FocusDistanceDiopters $originalCamera.focusDistanceDiopters | Out-Null
    & $control set-wb -Mode $(if($originalCamera.autoWhiteBalance){'Auto'}else{'Manual'}) -RedGain $originalCamera.redGain -BlueGain $originalCamera.blueGain | Out-Null
    & $control set-zoom -ZoomRatio $originalCamera.zoomRatio | Out-Null
    if ($null -eq $originalPreference) { $key.DeleteValue('SessionSettings',$false) }
    else { $key.SetValue('SessionSettings',$originalPreference,[Microsoft.Win32.RegistryValueKind]::String) }
    $key.Close()
}
