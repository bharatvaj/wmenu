$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NativeMenu {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
}
'@
$p = New-Object System.Diagnostics.Process
$p.StartInfo.FileName = (Join-Path $PSScriptRoot 'wmenu.exe')
$p.StartInfo.UseShellExecute = $false
$p.StartInfo.RedirectStandardInput = $true
$p.StartInfo.RedirectStandardOutput = $true
try {
    if (-not $p.Start()) { throw 'Could not start wmenu' }
    $p.StandardInput.WriteLine('first-streamed-line')
    $p.StandardInput.Flush()
    $count = -1
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 50
        $p.Refresh()
        $win = $p.MainWindowHandle
        if ($win -ne [IntPtr]::Zero) {
            $list = [NativeMenu]::FindWindowEx($win, [IntPtr]::Zero, 'ListBox', $null)
            if ($list -ne [IntPtr]::Zero) {
                $count = [NativeMenu]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
                if ($count -gt 0) { break }
            }
        }
    }
    if ($count -ne 1) { throw "Expected row before pipe closes; got $count, hwnd=$win, list=$list, exited=$($p.HasExited)" }
    for ($j = 0; $j -lt 3000; $j++) { $p.StandardInput.WriteLine("streamed-$j") }
    $p.StandardInput.Flush()
    $more = 0
    for ($i = 0; $i -lt 100 -and $more -lt 2049; $i++) {
        Start-Sleep -Milliseconds 50
        $more = [NativeMenu]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    }
    if ($more -lt 2049) { throw "Expected streamed list to grow while stdin open; got $more" }
    Write-Output "List reached $more rows before EOF."
} finally {
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
    $p.Dispose()
}
