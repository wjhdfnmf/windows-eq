# A responsive test window (own message loop, so Windows never "ghosts" it)
# covering a rectangle for a while. alpha < 0: normal window; 0..255:
# layered window with that constant alpha.
param([int]$X, [int]$Y, [int]$W, [int]$H, [int]$Alpha = -1, [int]$Seconds = 15)
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CW {
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int idx, int v);
    [DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@
# When this process is started hidden, Windows applies "hidden" to the first
# window shown: spend it on a throwaway window.
$dummy = New-Object System.Windows.Forms.Form
$dummy.ShowInTaskbar = $false
$dummy.Show(); $dummy.Hide(); $dummy.Dispose()

$f = New-Object System.Windows.Forms.Form
$f.FormBorderStyle = 'None'; $f.StartPosition = 'Manual'; $f.ShowInTaskbar = $false; $f.TopMost = $true
$f.BackColor = [System.Drawing.Color]::FromArgb(20, 22, 28)
$f.Bounds = [System.Drawing.Rectangle]::new($X, $Y, $W, $H)
$f.Add_Shown({
    # The process may be started hidden; Windows applies that to the first
    # window shown, so show this one explicitly.
    [void][CW]::ShowWindow($f.Handle, 5)
    if ($Alpha -ge 0) {
        [void][CW]::SetWindowLong($f.Handle, -20, ([CW]::GetWindowLong($f.Handle, -20) -bor 0x80000))
        [void][CW]::SetLayeredWindowAttributes($f.Handle, 0, [byte]$Alpha, 2)
    }
})
$t = New-Object System.Windows.Forms.Timer
$t.Interval = $Seconds * 1000
$t.Add_Tick({ $f.Close() })
$t.Start()
[System.Windows.Forms.Application]::Run($f)
