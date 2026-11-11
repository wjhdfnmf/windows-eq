# Lists the visible top-level windows above Windows EQ in z-order that overlap
# it, with the properties the app's "is it covered?" check looks at.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Z {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT r, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetLayeredWindowAttributes(IntPtr h, out uint key, out byte alpha, out uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int size);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT v, int size);
}
"@
$me = [Z]::FindWindow('WindowsEQ.Main', [NullString]::Value)
if ($me -eq [IntPtr]::Zero) { 'Windows EQ not running'; exit 1 }
if ($args -contains '-raise') {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class R { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f); }
"@
    [void][R]::SetWindowPos($me, [IntPtr](-1), 0, 0, 0, 0, 0x13)
    [void][R]::SetWindowPos($me, [IntPtr](-2), 0, 0, 0, 0, 0x13)
    Start-Sleep -Milliseconds 500
    'raised Windows EQ'
}
$c = New-Object Z+RECT; [void][Z]::GetClientRect($me, [ref]$c); [void][Z]::MapWindowPoints($me, [IntPtr]::Zero, [ref]$c, 2)
"Windows EQ client area: ($($c.L),$($c.T))-($($c.R),$($c.B))  visible=$([Z]::IsWindowVisible($me)) minimized=$([Z]::IsIconic($me))"
$w = [Z]::GetWindow($me, 3)  # GW_HWNDPREV: next window above
while ($w -ne [IntPtr]::Zero) {
    if ([Z]::IsWindowVisible($w) -and -not [Z]::IsIconic($w)) {
        $cl = 0; [void][Z]::DwmGetWindowAttribute($w, 14, [ref]$cl, 4)   # DWMWA_CLOAKED
        $r = New-Object Z+RECT
        if ([Z]::DwmGetWindowAttribute($w, 9, [ref]$r, 16) -ne 0) { [void][Z]::GetWindowRect($w, [ref]$r) }  # EXTENDED_FRAME_BOUNDS
        $overlaps = ($r.L -lt $c.R) -and ($r.R -gt $c.L) -and ($r.T -lt $c.B) -and ($r.B -gt $c.T)
        if ($cl -eq 0 -and $overlaps) {
            $ex = [Z]::GetWindowLong($w, -20)
            $layered = ($ex -band 0x80000) -ne 0; $transp = ($ex -band 0x20) -ne 0
            $alpha = 'n/a'
            if ($layered) { $k = 0; $a = [byte]0; $fl = 0; if ([Z]::GetLayeredWindowAttributes($w, [ref]$k, [ref]$a, [ref]$fl)) { $alpha = "alpha=$a flags=$fl" } else { $alpha = 'UpdateLayeredWindow' } }
            $cls = New-Object System.Text.StringBuilder 256; [void][Z]::GetClassName($w, $cls, 256)
            $tt = New-Object System.Text.StringBuilder 256; [void][Z]::GetWindowText($w, $tt, 256)
            $procId = 0; [void][Z]::GetWindowThreadProcessId($w, [ref]$procId)
            $pn = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
            $opaqueLayered = $layered -and ($alpha -match '^alpha=255 flags=2$')
            $counts = if ($transp -or ($layered -and -not $opaqueLayered)) { 'ignored (see-through)' } else { 'counts as covering' }
            $top = if (($ex -band 0x8) -ne 0) { 'TOPMOST ' } else { '' }
            $counts = $top + $counts
            "{0,-28} {1,-22} [{2}] rect ({3},{4})-({5},{6}) layered={7} {8} transparent={9} -> {10}" -f $cls, $pn, ($tt.ToString() -replace '(.{30}).*','$1...'), $r.L, $r.T, $r.R, $r.B, $layered, $alpha, $transp, $counts
        }
    }
    $w = [Z]::GetWindow($w, 3)
}
