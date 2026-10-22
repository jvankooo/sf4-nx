# Runs the PC test build against assets/game_root for a while and prints the tail of its log.
#   powershell -ExecutionPolicy Bypass -File tools\run_pc.ps1 [-Seconds 90]
#   -GameRoot build\sf4-nx\game_root  runs from another copy of the disc (the English-only SD copy, for example)
#   -Build pcvk  runs the Vulkan build (tools\build_pcvk.bat, the native renderer) instead of app\out\pc
param([int]$Seconds = 90, [string[]]$Extra = @(), [string]$GameRoot = '', [string]$Build = 'pc')
$root = Split-Path -Parent $PSScriptRoot
if (-not $GameRoot) { $GameRoot = "$root\assets\game_root" }
$dir = "$root\app\out\$Build"
Remove-Item "$dir\logs\*", "$dir\captures\*" -ErrorAction SilentlyContinue
# Through powershell -File an array arrives as one string joined by spaces: split it again (no value has spaces).
$args_ = @("--game_data_root=$GameRoot") + ($Extra | ForEach-Object { $_ -split '\s+' } | Where-Object { $_ })
$p = Start-Process "$dir\sf4.exe" -WorkingDirectory $dir -PassThru -ArgumentList $args_
$t = 0
while (-not $p.HasExited -and $t -lt $Seconds) { Start-Sleep 1; $t++ }
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
Write-Output "alive after ${t}s: $alive"
Get-ChildItem "$dir\logs\*.log" | ForEach-Object { Get-Content $_ -Tail 40 }
