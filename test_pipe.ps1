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
    $p.StandardInput.WriteLine('long-' + ('x' * 8192))
    for ($j = 0; $j -lt 3000; $j++) { $p.StandardInput.WriteLine("streamed-$j") }
    $p.StandardInput.Flush()
    $more = 0
    for ($i = 0; $i -lt 100 -and $more -lt 3002; $i++) {
        Start-Sleep -Milliseconds 50
        $more = [NativeMenu]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    }
    if ($more -ne 3002) { throw "Expected 3,002 rows including one long line before EOF; got $more" }
    $edit = [NativeMenu]::FindWindowEx($win, [IntPtr]::Zero, 'Edit', $null)
    if ($edit -eq [IntPtr]::Zero) { throw 'No edit control' }
    [void][NativeMenu]::SendMessage($list, 0x186, [IntPtr]1, [IntPtr]::Zero) # LB_SETCURSEL
    if ([NativeMenu]::SendMessage($list, 0x188, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ne 1) { throw 'Could not select the long line' }
    $output = $p.StandardOutput.ReadToEndAsync()
    [void][NativeMenu]::SendMessage($edit, 0x100, [IntPtr]13, [IntPtr]::Zero) # WM_KEYDOWN, Enter
    if (-not $p.WaitForExit(5000)) { throw 'wmenu did not exit after Enter' }
    if ($output.GetAwaiter().GetResult() -ne ('long-' + ('x' * 8192))) { throw 'Long row was not preserved on stdout' }
    Write-Output "List showed $more complete rows before EOF; long row output intact."
} finally {
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
    $p.Dispose()
}
