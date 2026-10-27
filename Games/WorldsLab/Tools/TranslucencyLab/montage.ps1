param([string]$Dir, [string[]]$Tags, [int[]]$Views = @(0, 1, 2, 3), [string]$Out, [double]$Scale = 0.5)
# One PNG of TranslucencyLab captures: a row per tag, a column per view, each at $Scale.
Add-Type -AssemblyName System.Drawing
$first = [System.Drawing.Image]::FromFile((Resolve-Path (Join-Path $Dir "$($Tags[0])_v$($Views[0]).bmp")))
$w = [int]($first.Width * $Scale); $h = [int]($first.Height * $Scale); $first.Dispose()
$bmp = New-Object System.Drawing.Bitmap ($w * $Views.Count), ($h * $Tags.Count)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object System.Drawing.Font "Arial", 12
for ($r = 0; $r -lt $Tags.Count; $r++) {
    for ($c = 0; $c -lt $Views.Count; $c++) {
        $path = Join-Path $Dir "$($Tags[$r])_v$($Views[$c]).bmp"
        if (-not (Test-Path $path)) { continue }
        $img = [System.Drawing.Image]::FromFile((Resolve-Path $path))
        $g.DrawImage($img, $c * $w, $r * $h, $w, $h)
        $img.Dispose()
        $g.DrawString("$($Tags[$r]) v$($Views[$c])", $font, [System.Drawing.Brushes]::Yellow, $c * $w + 4, $r * $h + 4)
    }
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output $Out
