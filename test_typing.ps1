# Windows integration test: keyboard latency while a large pipe remains open.
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NativeTyping {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr hwnd, uint msg, IntPtr wp, string text);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr ReadText(IntPtr hwnd, uint msg, IntPtr size, System.Text.StringBuilder text);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
}
'@
$p = [Diagnostics.Process]::new()
$p.StartInfo.FileName = (Join-Path $PSScriptRoot 'wmenu.exe')
$p.StartInfo.UseShellExecute = $false
$p.StartInfo.RedirectStandardInput = $true
$p.StartInfo.RedirectStandardOutput = $true
try {
    [void]$p.Start()
    for ($i = 0; $i -lt 20000; $i++) { $p.StandardInput.WriteLine("streamed-$i " + ('x' * 300)) }
    $p.StandardInput.Flush()
    $list = [IntPtr]::Zero
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 50
        $p.Refresh()
        $list = [NativeTyping]::FindWindowEx($p.MainWindowHandle, [IntPtr]::Zero, 'ListBox', $null)
        if ($list -ne [IntPtr]::Zero -and [NativeTyping]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ge 20000) { break }
    }
    if ($list -eq [IntPtr]::Zero -or [NativeTyping]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -lt 20000) { throw 'Stream did not load while pipe was open' }
    $edit = [NativeTyping]::FindWindowEx($p.MainWindowHandle, [IntPtr]::Zero, 'Edit', $null)
    if ($edit -eq [IntPtr]::Zero) { throw 'No edit control' }
    [void][NativeTyping]::SetText($edit, 0xC, [IntPtr]::Zero, 'streamed')
    [void][NativeTyping]::SendMessage($edit, 0xB1, [IntPtr]8, [IntPtr]8)
    $worst = 0
    for ($i = 1; $i -le 12; $i++) {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        [void][NativeTyping]::PostMessage($edit, 0x102, [IntPtr][int][char]'a', [IntPtr]::Zero)
        do {
            Start-Sleep -Milliseconds 1
            $buffer = [Text.StringBuilder]::new(80)
            [void][NativeTyping]::ReadText($edit, 0xD, [IntPtr]$buffer.Capacity, $buffer)
            if ($clock.ElapsedMilliseconds -gt 250) { throw "Typing stalled at character $i; got '$($buffer.ToString())'" }
        } while ($buffer.ToString() -ne ('streamed' + ('a' * $i)))
        if ($clock.ElapsedMilliseconds -gt $worst) { $worst = $clock.ElapsedMilliseconds }
    }
    Write-Output "Worst character latency: ${worst}ms with 20,000 streamed rows (pipe still open)."
} finally {
    if (!$p.HasExited) { $p.Kill(); $p.WaitForExit() }
    $p.Dispose()
}
