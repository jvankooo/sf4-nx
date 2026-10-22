# Puts the PNGs of app/out/pc/captures side by side (5 per row, 320 px wide) in app/out/pc/captures/sheet.png,
# each labelled with its file name, to see where a scripted run went in one image.
#   powershell -ExecutionPolicy Bypass -File tools\contact_sheet.ps1
param([string]$Folder = "$PSScriptRoot\..\app\out\pc\captures", [int]$PerRow = 5, [int]$Width = 320)
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem "$Folder\capture_*.png" | Sort-Object Name
if (-not $files) { Write-Output 'no captures'; exit 1 }
$first = [System.Drawing.Image]::FromFile($files[0].FullName)
$h = [int]($Width * $first.Height / $first.Width)
$first.Dispose()
$rows = [Math]::Ceiling($files.Count / $PerRow)
$sheet = New-Object System.Drawing.Bitmap ($PerRow * $Width), ($rows * ($h + 18))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font 'Arial', 10
for ($i = 0; $i -lt $files.Count; $i++) {
    $img = [System.Drawing.Image]::FromFile($files[$i].FullName)
    $x = ($i % $PerRow) * $Width
    $y = [Math]::Floor($i / $PerRow) * ($h + 18)
    $g.DrawImage($img, $x, $y + 18, $Width, $h)
    $g.DrawString($files[$i].BaseName, $font, [System.Drawing.Brushes]::White, $x + 2, $y + 2)
    $img.Dispose()
}
$sheet.Save("$Folder\sheet.png", [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "$Folder\sheet.png"
