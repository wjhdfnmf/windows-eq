# Reports the state of a per-user Windows EQ installation.
$exe = Join-Path $env:LOCALAPPDATA 'Programs\WindowsEQ\WindowsEQ.exe'
$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Windows EQ.lnk'

Write-Output '--- files'
foreach ($f in @($exe, $lnk)) {
    if (Test-Path -LiteralPath $f) {
        $i = Get-Item -LiteralPath $f
        Write-Output ("OK   {0}  ({1:N0} bytes, {2})" -f $f, $i.Length, $i.LastWriteTime)
    } else {
        Write-Output "MISSING  $f"
    }
}
if (Test-Path -LiteralPath $exe) {
    Write-Output ("     version {0}" -f (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion)
}
if (Test-Path -LiteralPath $lnk) {
    $sh = New-Object -ComObject WScript.Shell
    Write-Output ("     shortcut target: {0}" -f $sh.CreateShortcut($lnk).TargetPath)
}

Write-Output '--- Apps & features entry'
$u = Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsEQ' -ErrorAction SilentlyContinue
if ($u) {
    Write-Output ("OK   {0} {1}" -f $u.DisplayName, $u.DisplayVersion)
    Write-Output ("     uninstall: {0}" -f $u.UninstallString)
} else {
    Write-Output 'MISSING  uninstall entry'
}

Write-Output '--- start with Windows'
$run = (Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).WindowsEQ
if ($run) { Write-Output "ON   $run" } else { Write-Output 'OFF' }

Write-Output '--- running'
$procs = Get-Process WindowsEQ -ErrorAction SilentlyContinue
if ($procs) {
    foreach ($p in $procs) {
        Write-Output ("PID {0}  {1}  window: '{2}'" -f $p.Id, $p.Path, $p.MainWindowTitle)
    }
} else {
    Write-Output 'not running'
}

Write-Output '--- settings'
$cfg = Join-Path $env:APPDATA 'WindowsEQ\config.ini'
if (Test-Path -LiteralPath $cfg) { Get-Content -LiteralPath $cfg } else { Write-Output 'no config.ini yet' }
