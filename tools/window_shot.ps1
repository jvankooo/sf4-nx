# Screenshots of the game window itself (what the player sees), for renderers whose output the in-game capture
# (sf4_capture_every_s, the SDK presenter's guest output) does not see: starts the PC build like run_pc.ps1 and saves
# <build>\shots\shot_<s>s.png at the given seconds.
#   powershell -ExecutionPolicy Bypass -File tools\window_shot.ps1 -Build pcvk -At 10,20,30 -Extra "..."
param([string]$Build = 'pcvk', [int[]]$At = @(10), [string[]]$Extra = @(), [string]$GameRoot = '')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Win]::SetProcessDPIAware() | Out-Null
$root = Split-Path -Parent $PSScriptRoot
if (-not $GameRoot) { $GameRoot = "$root\assets\game_root" }
$dir = "$root\app\out\$Build"
New-Item -ItemType Directory -Force "$dir\shots" | Out-Null
Remove-Item "$dir\shots\*", "$dir\logs\*" -ErrorAction SilentlyContinue
$args_ = @("--game_data_root=$GameRoot") + ($Extra | ForEach-Object { $_ -split '\s+' } | Where-Object { $_ })
$p = Start-Process "$dir\sf4.exe" -WorkingDirectory $dir -PassThru -ArgumentList $args_
$start = Get-Date
foreach ($s in ($At | Sort-Object)) {
  while (((Get-Date) - $start).TotalSeconds -lt $s -and -not $p.HasExited) { Start-Sleep -Milliseconds 200 }
  if ($p.HasExited) { break }
  $p.Refresh()
  $h = $p.MainWindowHandle
  [Win]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 300
  $r = New-Object Win+RECT; [Win]::GetClientRect($h, [ref]$r) | Out-Null
  $pt = New-Object Win+POINT; [Win]::ClientToScreen($h, [ref]$pt) | Out-Null
  $w = $r.R - $r.L; $hgt = $r.B - $r.T
  if ($w -le 0 -or $hgt -le 0) { continue }
  $bmp = New-Object System.Drawing.Bitmap $w, $hgt
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
  $bmp.Save("$dir\shots\shot_$('{0:D3}' -f $s)s.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-ChildItem "$dir\shots" | ForEach-Object { $_.FullName }
