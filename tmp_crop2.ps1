$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$srcPath = "C:\Users\admin\.zcode\cli\image-cache\sess_6dab7bad-9037-4a48-8027-12e12f752706\image-46b0158027184f88596946ece01503d0.png"
$src = New-Object System.Drawing.Bitmap -ArgumentList $srcPath
$outDir = Join-Path $env:TEMP "xjscrop"
function CropZoom([int]$x, [int]$y, [int]$w, [int]$h, [int]$scale, [string]$name) {
  $bw = $w * $scale
  $bh = $h * $scale
  $dst = New-Object System.Drawing.Bitmap -ArgumentList $bw, $bh
  $g = [System.Drawing.Graphics]::FromImage($dst)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
  $srcRect = New-Object System.Drawing.Rectangle -ArgumentList $x, $y, $w, $h
  $dstRect = New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $bw, $bh
  $g.DrawImage($src, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
  $g.Dispose()
  $dst.Save((Join-Path $outDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
  $dst.Dispose()
}
CropZoom 0 620 559 107 3 "bottom.png"
CropZoom 0 38 559 34 3 "header.png"
CropZoom 440 496 119 46 5 "fcol_folder.png"
$src.Dispose()
Write-Output "OK"
