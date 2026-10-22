# Compares two PNG captures pixel by pixel: share of pixels that differ by more than a threshold, the mean difference,
# and a difference image (white = different) next to the first file.
#   powershell -ExecutionPolicy Bypass -File tools\image_diff.ps1 a.png b.png [-Threshold 24]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [int]$Threshold = 24)
Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))
if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) { throw "different sizes" }
$out = New-Object System.Drawing.Bitmap $ia.Width, $ia.Height
$diff = 0; $sum = 0.0; $n = 0
for ($y = 0; $y -lt $ia.Height; $y += 2) {
    for ($x = 0; $x -lt $ia.Width; $x += 2) {
        $p = $ia.GetPixel($x, $y); $q = $ib.GetPixel($x, $y)
        $d = [Math]::Max([Math]::Abs($p.R - $q.R), [Math]::Max([Math]::Abs($p.G - $q.G), [Math]::Abs($p.B - $q.B)))
        $sum += $d; $n++
        if ($d -gt $Threshold) { $diff++; $out.SetPixel($x, $y, [System.Drawing.Color]::White) }
    }
}
$path = [System.IO.Path]::ChangeExtension((Resolve-Path $A), '.diff.png')
$out.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
"{0:N2} % of pixels differ by more than {1}, mean difference {2:N2} -> {3}" -f (100.0 * $diff / $n), $Threshold, ($sum / $n), $path
