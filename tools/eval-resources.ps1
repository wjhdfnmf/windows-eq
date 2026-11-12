# Windows EQ resource evaluation. For each window state it measures the
# frames the app actually drew (asked from the app itself), plus its GPU
# (3D engine) and CPU use, and checks them against targets.
# Play some audio through Windows EQ first.
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class E {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr h, int idx, int v);
    [DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@

$h = [E]::FindWindow('WindowsEQ.Main', [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { 'Windows EQ is not running'; exit 1 }
$diag = [E]::RegisterWindowMessage('WindowsEQ.Diagnostics')
function Frames { [long][E]::SendMessage($h, $diag, [IntPtr]0, [IntPtr]0) }
function Flags { [long][E]::SendMessage($h, $diag, [IntPtr]1, [IntPtr]0) }

$procId = 0; [void][E]::GetWindowThreadProcessId($h, [ref]$procId)
"Windows EQ {0} (PID {1})" -f (Get-Process -Id $procId).MainModule.FileVersionInfo.ProductVersion, $procId
if ([E]::IsIconic($h)) { [void][E]::ShowWindow($h, 9); Start-Sleep 1 }

function Raise-Eq {   # on top of other windows, without taking focus
    $f = 0x0001 -bor 0x0002 -bor 0x0010
    [void][E]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, $f)
    [void][E]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, $f)
    Start-Sleep -Milliseconds 700
}
Raise-Eq
$fl = Flags
"audio reaching the app: {0}   routing running: {1}   renderer: {2}" -f [bool]($fl -band 1), [bool]($fl -band 2),
    $(if ($fl -band 16) { 'low-GPU (flip model)' } else { 'compatible (window target)' })
$dwmId = (Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id
if (-not ($fl -band 1)) { 'WARNING: no audio is reaching Windows EQ; the visible-phase numbers will show the idle rate.' }

function Get-EqUsage([int]$seconds) {
    $p = Get-Process -Id $procId
    $script:activeAtStart = [bool]((Flags) -band 8)
    $c0 = $p.TotalProcessorTime.TotalMilliseconds
    $f0 = Frames
    # The compositor (dwm.exe) is reported too: with the flip model it does
    # the window's final copy, so that work doesn't show up under the app.
    $eqPath = "\GPU Engine(pid_$($procId)_*engtype_3D)\Utilization Percentage"
    $dwmPath = "\GPU Engine(pid_$($dwmId)_*engtype_3D)\Utilization Percentage"
    $samples = Get-Counter -Counter @($eqPath, $dwmPath) -SampleInterval 1 -MaxSamples $seconds -ErrorAction SilentlyContinue
    $f1 = Frames
    $p.Refresh()
    $sum = { param($pid_) (@(foreach ($s in $samples) {
        ($s.CounterSamples | Where-Object Path -like "*pid_$($pid_)_*" | Measure-Object CookedValue -Sum).Sum
    }) | Measure-Object -Average).Average }
    [pscustomobject]@{
        Fps = [math]::Round(($f1 - $f0) / $seconds, 1)
        Gpu = [math]::Round([double](& $sum $procId), 2)
        Dwm = [math]::Round([double](& $sum $dwmId), 2)
        Cpu = [math]::Round(($p.TotalProcessorTime.TotalMilliseconds - $c0) / ($seconds * 10), 2)
        Audio = [bool]((Flags) -band 1)
        Active = $script:activeAtStart
    }
}

function Cover([int]$alpha) {
    # The cover runs in its own process with a message loop: a window that
    # stops pumping messages gets replaced by a see-through "ghost".
    $r = New-Object E+RECT; [void][E]::GetWindowRect($h, [ref]$r)
    $script = Join-Path $PSScriptRoot 'cover-window.ps1'
    $p = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"",
        ($r.L - 20), ($r.T - 20), ($r.R - $r.L + 40), ($r.B - $r.T + 40), $alpha, 25)
    # Wait until the cover window exists (PowerShell + C# compile take a few s):
    # an opaque cover makes the app report "not seen"; a see-through one
    # never does, so give it the same time.
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 10) {
        Start-Sleep -Milliseconds 250
        if ($alpha -ne 160 -and -not ((Flags) -band 4)) { break }
    }
    if ($alpha -eq 160) { Start-Sleep -Seconds 6 }
    Start-Sleep -Seconds 1
    "  cover ready after {0:N1} s" -f ((Get-Date) - $t0).TotalSeconds | Write-Host
    return $p
}

$results = @()
function Phase($name, [scriptblock]$setup, [scriptblock]$teardown, [scriptblock]$check, [string]$target) {
    Raise-Eq
    $state = & $setup
    $m = Get-EqUsage 10
    & $teardown $state
    $ok = & $check $m
    $script:results += [pscustomobject]@{ Phase = $name; FPS = $m.Fps; 'GPU %' = $m.Gpu; 'DWM GPU %' = $m.Dwm; 'CPU %' = $m.Cpu
        Audio = $m.Audio; Active = $m.Active; Target = $target; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }) }
    Start-Sleep -Seconds 1
}
$closeCover = { param($p) if ($p -and -not $p.HasExited) { $p.Kill() } }
$none = { }
# Visible with audio: 24 fps when it's the active window, 12 fps otherwise.
$visibleCheck = { param($m)
    if (-not $m.Audio) { return $m.Fps -le 4 -and $m.Gpu -le 2 }
    $fpsOk = if ($m.Active) { $m.Fps -ge 18 -and $m.Fps -le 26 } else { $m.Fps -ge 9 -and $m.Fps -le 14 }
    $fpsOk -and $m.Gpu -le 6 -and $m.Cpu -le 4 }
$hiddenCheck = { param($m) $m.Fps -le 0.2 -and $m.Gpu -le 0.5 -and $m.Cpu -le 1.5 }

Phase 'visible' { $null } $none $visibleCheck '24 fps active / 12 inactive, GPU<=6%, CPU<=4%'
Phase 'covered: normal window' { Cover -1 } $closeCover $hiddenCheck '0 fps, GPU<=0.5%, CPU<=1.5%'
Phase 'covered: opaque layered window' { Cover 255 } $closeCover $hiddenCheck '0 fps, GPU<=0.5%, CPU<=1.5%'
Phase 'covered: semi-transparent window' { Cover 160 } $closeCover { param($m) $m.Fps -ge 1.5 } 'keeps drawing (>=1.5 fps)'
Phase 'minimized' { [void][E]::ShowWindow($h, 6); Start-Sleep 2 } { [void][E]::ShowWindow($h, 9) } $hiddenCheck '0 fps, GPU<=0.5%, CPU<=1.5%'

$results | Format-Table -AutoSize | Out-String -Width 220
$fails = @($results | Where-Object Result -eq 'FAIL').Count
if ($fails) { "$fails phase(s) failed" } else { 'all phases passed' }
