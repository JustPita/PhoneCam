<#
.SYNOPSIS
  Génère pc\app\phonecam.ico (16 à 256 px) : caméra blanche sur carré bleu arrondi,
  même dessin que l'icône Android (android\app\src\main\res\drawable\ic_phonecam.xml).
#>
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\app\phonecam.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

function New-RoundRect([System.Drawing.RectangleF]$r, [float]$rad) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $rad * 2
    $p.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $p.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $p.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $p.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $s / 24.0  # le dessin est pensé sur une grille 24×24

    # Fond : dégradé bleu
    $bg = New-RoundRect (New-Object System.Drawing.RectangleF (1 * $k), (1 * $k), (22 * $k), (22 * $k)) (5 * $k)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $s),
        [System.Drawing.Color]::FromArgb(255, 66, 165, 245), [System.Drawing.Color]::FromArgb(255, 21, 101, 192))
    $g.FillPath($brush, $bg)

    # Corps de la caméra
    $white = [System.Drawing.Brushes]::White
    $body = New-RoundRect (New-Object System.Drawing.RectangleF (4.5 * $k), (7.5 * $k), (11 * $k), (9 * $k)) (1.8 * $k)
    $g.FillPath($white, $body)
    # Objectif (triangle à droite)
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF (15 * $k), (11 * $k)),
        (New-Object System.Drawing.PointF (19.5 * $k), (8 * $k)),
        (New-Object System.Drawing.PointF (19.5 * $k), (16 * $k)),
        (New-Object System.Drawing.PointF (15 * $k), (13 * $k)))
    $g.FillPolygon($white, $pts)
    # Pastille « enregistrement » (sauf en très petit)
    if ($s -ge 32) {
        $red = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 229, 57, 53))
        $g.FillEllipse($red, 6.5 * $k, 9.5 * $k, 2.4 * $k, 2.4 * $k)
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

# Conteneur ICO avec entrées PNG
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
Write-Host "Icône écrite : $out"
