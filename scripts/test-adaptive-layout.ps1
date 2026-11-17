[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AdaptiveLayoutTest {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent,IntPtr after,string type,string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr parent,int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd,out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int width,int height,uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd,uint message,IntPtr w,IntPtr l);
    public static IntPtr Find(uint target) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((hwnd,data)=>{ uint pid; GetWindowThreadProcessId(hwnd,out pid);
            if(pid==target && GetDlgItem(hwnd,100)!=IntPtr.Zero) {result=hwnd;return false;} return true;},IntPtr.Zero);
        return result;
    }
    public static Rect Bounds(IntPtr hwnd) { Rect r; GetWindowRect(hwnd,out r); return r; }
    public static void Dpi(IntPtr hwnd,int dpi) {
        Rect r=Bounds(hwnd);
        IntPtr p=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Rect)));
        try { Marshal.StructureToPtr(r,p,false); SendMessage(hwnd,0x02E0,(IntPtr)(dpi|(dpi<<16)),p); }
        finally {Marshal.FreeHGlobal(p);}
    }
}
'@

$window = [IntPtr]::Zero
$process = Start-Process (Join-Path $root 'windows\build\bin\StratoCam.exe') -WindowStyle Hidden -PassThru
$checks = [Collections.Generic.List[object]]::new()
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        if ($process.HasExited) { throw 'Application exited during layout test.' }
        $window = [AdaptiveLayoutTest]::Find([uint32]$process.Id)
    } while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero) { throw 'Application window not found.' }
    $panel = [AdaptiveLayoutTest]::FindWindowEx($window,[IntPtr]::Zero,'StratoCamControlsPanel',$null)
    $preview = [AdaptiveLayoutTest]::FindWindowEx($window,[IntPtr]::Zero,'AndroidCamPreviewWindow',$null)
    if ($panel -eq [IntPtr]::Zero -or $preview -eq [IntPtr]::Zero) { throw 'Panel or preview missing.' }
    # Exercise the DPI-change handler without changing the user's monitor settings.
    foreach ($dpi in 96,120,144,192) {
        [AdaptiveLayoutTest]::Dpi($window,$dpi)
        foreach ($size in @(@(720,480),@(900,600),@(1100,720),@(1460,920))) {
            [void][AdaptiveLayoutTest]::SetWindowPos($window,[IntPtr]::Zero,0,0,
                [int]($size[0]*$dpi/96),[int]($size[1]*$dpi/96),0x0016)
            $panelBounds = [AdaptiveLayoutTest]::Bounds($panel)
            $previewBounds = [AdaptiveLayoutTest]::Bounds($preview)
            if ($previewBounds.Right -gt $panelBounds.Left -or $previewBounds.Bottom -le $previewBounds.Top) {
                throw "Preview/panel overlap at DPI=$dpi size=$size"
            }
            foreach ($id in 102,105,107,123,124) {
                $child = [AdaptiveLayoutTest]::GetDlgItem($panel,$id)
                if ($child -eq [IntPtr]::Zero) { throw "Panel control $id missing." }
                $bounds = [AdaptiveLayoutTest]::Bounds($child)
                if ($bounds.Left -lt $panelBounds.Left -or $bounds.Right -gt $panelBounds.Right) {
                    throw "Panel control $id exceeds horizontal bounds at DPI=$dpi"
                }
            }
            [void][AdaptiveLayoutTest]::SendMessage($panel,0x0115,[IntPtr]7,[IntPtr]::Zero) # SB_BOTTOM
            $reset = [AdaptiveLayoutTest]::Bounds([AdaptiveLayoutTest]::GetDlgItem($panel,114))
            if ($reset.Top -lt $panelBounds.Top -or $reset.Bottom -gt $panelBounds.Bottom) {
                throw "Reset is unreachable after scrolling at DPI=$dpi size=$size"
            }
            [void][AdaptiveLayoutTest]::SendMessage($panel,0x0115,[IntPtr]6,[IntPtr]::Zero) # SB_TOP
            $checks.Add([pscustomobject]@{dpi=$dpi; logicalWindow=$size; passed=$true})
        }
    }
    $output = Join-Path $root 'captures\adaptive-layout-test.json'
    $checks | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $output -Encoding UTF8
    Write-Host "PASS: $($checks.Count) layout/DPI scenarios; controls reachable, preview isolated. $output"
} finally {
    if ($window -ne [IntPtr]::Zero) {
        [void][AdaptiveLayoutTest]::PostMessage($window,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)
    }
    if (-not $process.WaitForExit(25000)) { Stop-Process -Id $process.Id -Force }
}
