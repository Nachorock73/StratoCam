param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\windows\build\bin\StratoCam.exe'),
    [switch]$ExerciseStreamModes,
    [string]$ScreenshotPath,
    [ValidateRange(1, 100)] [int]$Cycles = 30
)

$ErrorActionPreference = 'Stop'
$Executable = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $Executable)) {
    throw "StratoCam executable not found: $Executable"
}

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class StratoCamResizeNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    [DllImport("user32.dll", SetLastError=true)] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError=true)] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref POINT point, uint count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr parent, int id);
}
'@

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$BM_CLICK = 0x00F5
$CB_SETCURSEL = 0x014E
$CBN_SELCHANGE = 1
$SW_RESTORE = 9
$SW_MAXIMIZE = 3
$SWP_NOACTIVATE = 0x0010
$SWP_NOMOVE = 0x0002
$SWP_NOSIZE = 0x0001
$SWP_SHOWWINDOW = 0x0040
$HWND_TOPMOST = [IntPtr](-1)
$HWND_NOTOPMOST = [IntPtr](-2)
$ID_START = 100
$ID_ORIENTATION = 115
$ID_SCALE = 116
$ID_MODE = 121

function Send-ComboSelection([IntPtr]$Window, [int]$Id, [int]$Selection) {
    $combo = [StratoCamResizeNative]::GetDlgItem($Window, $Id)
    if ($combo -eq [IntPtr]::Zero) { throw "Control $Id was not found" }
    [void][StratoCamResizeNative]::SendMessage($combo, $CB_SETCURSEL, [IntPtr]$Selection, [IntPtr]::Zero)
    $command = [IntPtr](($CBN_SELCHANGE -shl 16) -bor $Id)
    [void][StratoCamResizeNative]::SendMessage($Window, $WM_COMMAND, $command, $combo)
}

function Assert-PreviewBounds([IntPtr]$Window, [string]$State) {
    $preview = [StratoCamResizeNative]::FindWindowEx($Window, [IntPtr]::Zero, 'AndroidCamPreviewWindow', $null)
    if ($preview -eq [IntPtr]::Zero) { throw "Preview window was not found ($State)" }
    $client = New-Object StratoCamResizeNative+RECT
    $previewClient = New-Object StratoCamResizeNative+RECT
    if (-not [StratoCamResizeNative]::GetClientRect($Window, [ref]$client) -or
        -not [StratoCamResizeNative]::GetClientRect($preview, [ref]$previewClient)) {
        throw "Unable to read window geometry ($State)"
    }
    $origin = New-Object StratoCamResizeNative+POINT
    [void][StratoCamResizeNative]::MapWindowPoints($preview, $Window, [ref]$origin, 1)
    $previewRight = $origin.X + $previewClient.Right
    $previewBottom = $origin.Y + $previewClient.Bottom
    $panel = [StratoCamResizeNative]::FindWindowEx($Window, [IntPtr]::Zero, 'StratoCamControlsPanel', $null)
    $panelOrigin = New-Object StratoCamResizeNative+POINT
    [void][StratoCamResizeNative]::MapWindowPoints($panel, $Window, [ref]$panelOrigin, 1)
    $panelX = $panelOrigin.X
    if ($panel -eq [IntPtr]::Zero -or $origin.X -lt 0 -or $origin.Y -lt $panelOrigin.Y -or
        $previewRight -gt $panelX -or $previewBottom -gt $client.Bottom) {
        throw "Preview escaped its container at $State`: preview=$($origin.X),$($origin.Y),$previewRight,$previewBottom client=$($client.Right)x$($client.Bottom) panelX=$panelX"
    }
}

$process = Start-Process -FilePath $Executable -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $window = [StratoCamResizeNative]::FindWindow('AndroidCamMainWindow', 'StratoCam')
    } while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'StratoCam main window did not appear' }

    $start = [StratoCamResizeNative]::GetDlgItem($window, $ID_START)
    if ($start -ne [IntPtr]::Zero) { [void][StratoCamResizeNative]::SendMessage($start, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) }

    foreach ($size in @(@(1080,800), @(1460,920), @(1900,1000), @(1200,1200), @(2200,900))) {
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_RESTORE)
        [void][StratoCamResizeNative]::SetWindowPos($window, [IntPtr]::Zero, 30, 30, $size[0], $size[1], $SWP_NOACTIVATE)
        Start-Sleep -Milliseconds 150
        Assert-PreviewBounds $window "$($size[0])x$($size[1])"
    }

    1..$Cycles | ForEach-Object {
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_MAXIMIZE)
        Start-Sleep -Milliseconds 80
        Assert-PreviewBounds $window "maximize $_"
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_RESTORE)
        Start-Sleep -Milliseconds 80
        Assert-PreviewBounds $window "restore $_"
    }

    1..$Cycles | ForEach-Object {
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_MAXIMIZE)
        Start-Sleep -Milliseconds 80
        Assert-PreviewBounds $window "maximize-before-minimize $_"
        [void][StratoCamResizeNative]::ShowWindow($window, 6)
        Start-Sleep -Milliseconds 80
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_RESTORE)
        Start-Sleep -Milliseconds 80
        Assert-PreviewBounds $window "minimize-restore $_"
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_RESTORE)
        Start-Sleep -Milliseconds 80
    }

    foreach ($orientation in 0..5) {
        Send-ComboSelection $window $ID_ORIENTATION $orientation
        foreach ($scale in 0..2) {
            Send-ComboSelection $window $ID_SCALE $scale
            Start-Sleep -Milliseconds 80
            Assert-PreviewBounds $window "orientation=$orientation scale=$scale"
        }
    }

    if ($ExerciseStreamModes) {
        foreach ($mode in 0..1) {
            Send-ComboSelection $window $ID_MODE $mode
            Start-Sleep -Seconds 3
            Assert-PreviewBounds $window "stream mode=$mode"
        }
    }

    if ($ScreenshotPath) {
        [void][StratoCamResizeNative]::ShowWindow($window, $SW_MAXIMIZE)
        Start-Sleep -Seconds 2
        Assert-PreviewBounds $window 'final maximized capture'
        $windowRect = New-Object StratoCamResizeNative+RECT
        if (-not [StratoCamResizeNative]::GetWindowRect($window, [ref]$windowRect)) {
            throw 'Unable to read final window rectangle'
        }
        Add-Type -AssemblyName System.Drawing
        $bitmap = New-Object System.Drawing.Bitmap ($windowRect.Right - $windowRect.Left), ($windowRect.Bottom - $windowRect.Top)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            [void][StratoCamResizeNative]::SetWindowPos(
                $window, $HWND_TOPMOST, 0, 0, 0, 0, $SWP_NOMOVE -bor $SWP_NOSIZE -bor $SWP_SHOWWINDOW)
            [void][StratoCamResizeNative]::SetForegroundWindow($window)
            Start-Sleep -Milliseconds 500
            $graphics.CopyFromScreen($windowRect.Left, $windowRect.Top, 0, 0, $bitmap.Size)
            $resolvedScreenshot = [System.IO.Path]::GetFullPath($ScreenshotPath)
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($resolvedScreenshot)) | Out-Null
            $bitmap.Save($resolvedScreenshot, [System.Drawing.Imaging.ImageFormat]::Png)
            Write-Host "Screenshot: $resolvedScreenshot"
        } finally {
            [void][StratoCamResizeNative]::SetWindowPos(
                $window, $HWND_NOTOPMOST, 0, 0, 0, 0, $SWP_NOMOVE -bor $SWP_NOSIZE)
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }

    Write-Host 'PASS: preview remained inside its container for all resize/orientation/scale states.'
} finally {
    if ($window -ne [IntPtr]::Zero) {
        [void][StratoCamResizeNative]::PostMessage($window, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
}
