# Shows the real (unsandboxed) renderer settings, recent renderer log lines
# and recent graphics-driver events. Usage: .\tools\renderer-state.ps1 [hours]
param([double]$Hours = 6)
$dir = Join-Path $env:APPDATA 'WindowsEQ'
$cfg = Join-Path $dir 'config.ini'
Write-Host "== config.ini ($cfg)"
if (Test-Path $cfg) { Get-Content $cfg | Where-Object { $_ -match 'renderer|flipCleanExit' } } else { Write-Host '(missing)' }
$log = Join-Path $dir 'error.log'
Write-Host "== error.log (renderer lines)"
if (Test-Path $log) { Get-Content $log | Select-String 'renderer' | Select-Object -Last 10 | ForEach-Object { $_.Line } }
Write-Host "== running WindowsEQ"
Get-Process WindowsEQ -ErrorAction SilentlyContinue | Select-Object Id, StartTime, Path | Format-Table -AutoSize | Out-String -Width 200
Write-Host "== graphics / crash events in the last $Hours h"
$since = (Get-Date).AddHours(-$Hours)
Get-WinEvent -FilterHashtable @{LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'nvlddmkm|Display|dxgkrnl|WHEA|amdkmdag|igfx' -or $_.Id -in 4101, 1001, 41 } |
    Select-Object TimeCreated, ProviderName, Id | Format-Table -AutoSize | Out-String -Width 200
