# Generates dashboard/framesmooth.ico (same look as the SteamVR dashboard thumbnail:
# a blue rounded tile with two white "frames"). Run once; the .ico is committed to the repository.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\dashboard\framesmooth.ico'

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); return $p
}

$sizes = 16, 24, 32, 48, 64, 256
$images = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.Clear([System.Drawing.Color]::Transparent)
    $blue = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 38, 132, 255))
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $faint = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 255, 255, 255))
    $g.FillPath($blue, (New-RoundRect 0 0 $s $s ($s * 0.2)))
    # two frames: a faint "synthesized" one behind, a solid "real" one in front
    $fw = $s * 0.42; $fh = $s * 0.36
    $g.FillPath($faint, (New-RoundRect ($s * 0.14) ($s * 0.22) $fw $fh ($s * 0.07)))
    $g.FillPath($white, (New-RoundRect ($s * 0.44) ($s * 0.42) $fw $fh ($s * 0.07)))
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    , $ms.ToArray()
}

# ICO container with PNG-compressed entries
$fs = [System.IO.File]::Create($out); $bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $images[$i].Length
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$len); $bw.Write([uint32]$offset); $offset += $len
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Close()
Write-Host "Wrote $out"
