# TouchCursorFix icon generator - draws the icon with GDI+ at multiple sizes and
# packs TouchCursorFix.ico (16/20/24/32/48/64 as BMP frames + 256 as PNG frame).
# Preview PNGs (icon on a checkerboard, small sizes upscaled) go to %TEMP%.
# Usage: powershell -ExecutionPolicy Bypass -File make_icon.ps1
param(
    [string]$IcoPath    = (Join-Path $PSScriptRoot 'TouchCursorFix.ico'),
    [string]$PreviewDir = (Join-Path $env:TEMP 'TouchCursorFix_icon_previews')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $PreviewDir -Force | Out-Null

function Draw-Icon([int]$s) {
    $f = $s / 32.0
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # --- rounded-square background, blue vertical gradient ---
    $inset = [float](0.5 * $f); $r = [float](7.5 * $f)
    $x0 = $inset; $y0 = $inset; $x1 = $s - $inset; $y1 = $s - $inset; $d = [float](2 * $r)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x0,      $y0,      $d, $d, 180, 90)
    $path.AddArc($x1 - $d, $y0,      $d, $d, 270, 90)
    $path.AddArc($x1 - $d, $y1 - $d, $d, $d,   0, 90)
    $path.AddArc($x0,      $y1 - $d, $d, $d,  90, 90)
    $path.CloseFigure()

    $cTop = [System.Drawing.Color]::FromArgb(255, 0x2E, 0x86, 0xF5)
    $cBot = [System.Drawing.Color]::FromArgb(255, 0x0B, 0x39, 0x8C)
    $rectF = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rectF, $cTop, $cBot, [float]90)
    $g.FillPath($brush, $path)
    $penB = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 255, 255, 255), [float][Math]::Max(1.0, 1.0 * $f))
    $g.DrawPath($penB, $path)

    # --- touch ripple, bottom-right ---
    $g.SetClip($path)
    $cx = 22.5 * $f; $cy = 22.5 * $f
    $ringPen1 = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(215, 0x9F, 0xE2, 0xFF), [float][Math]::Max(1.0, 1.7 * $f))
    $ringPen2 = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(120, 0x9F, 0xE2, 0xFF), [float][Math]::Max(1.0, 1.5 * $f))
    $r1 = 5.6 * $f; $r2 = 9.3 * $f
    $g.DrawEllipse($ringPen1, [float]($cx - $r1), [float]($cy - $r1), [float](2 * $r1), [float](2 * $r1))
    if ($s -ge 24) {
        $g.DrawEllipse($ringPen2, [float]($cx - $r2), [float]($cy - $r2), [float](2 * $r2), [float](2 * $r2))
    }
    $dotR = 2.6 * $f
    $dotColor = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xF4, 0xFF)
    if ($s -lt 24) { $dotR = 3.0 * $f; $dotColor = [System.Drawing.Color]::White }   # punchier at tray size
    $dotBrush = New-Object System.Drawing.SolidBrush($dotColor)
    $g.FillEllipse($dotBrush, [float]($cx - $dotR), [float]($cy - $dotR), [float](2 * $dotR), [float](2 * $dotR))
    $g.ResetClip()

    # --- classic arrow cursor, upper-left (white with dark navy outline) ---
    $ax = 7.5; $ay = 5.5; $k = 1.06
    $raw = @(
        @(0.0, 0.0), @(0.0, 14.4), @(3.4, 11.2), @(5.7, 16.1),
        @(8.3, 14.9), @(6.0, 10.1), @(10.8, 10.1)
    )
    $pts = New-Object 'System.Drawing.PointF[]' $raw.Count
    for ($i = 0; $i -lt $raw.Count; $i++) {
        $pts[$i] = New-Object System.Drawing.PointF(
            [float](($ax + $raw[$i][0] * $k) * $f),
            [float](($ay + $raw[$i][1] * $k) * $f))
    }
    $outlinePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0x0A, 0x1F, 0x45), [float][Math]::Max(1.0, 2.2 * $f))
    $outlinePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPolygon($outlinePen, $pts)
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $g.FillPolygon($white, $pts)

    $g.Dispose()
    return $bmp
}

# --- preview helper: icon on a checkerboard, optionally nearest-neighbor upscaled ---
function Save-Preview([System.Drawing.Bitmap]$bmp, [int]$scale, [string]$file) {
    $w = $bmp.Width * $scale
    $out = New-Object System.Drawing.Bitmap($w, $w)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $b1 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 205, 205, 205))
    $b2 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 242, 242, 242))
    $cell = 16
    for ($y = 0; $y -lt $w; $y += $cell) {
        for ($x = 0; $x -lt $w; $x += $cell) {
            $br = if ((($x + $y) / $cell) % 2 -eq 0) { $b1 } else { $b2 }
            $g.FillRectangle($br, $x, $y, $cell, $cell)
        }
    }
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($bmp, 0, 0, $w, $w)
    $g.Dispose()
    $out.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose()
}

# --- ICO frame builders ---
function Get-BmpFrame([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $bd.Stride
    $px = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $px, 0, $stride * $h)
    $bmp.UnlockBits($bd)

    $maskRow = [int]([math]::Ceiling($w / 32.0) * 4)
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int]40); $bw.Write([int]$w); $bw.Write([int]($h * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($w * $h * 4 + $maskRow * $h))
    $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $h - 1; $y -ge 0; $y--) { $bw.Write($px, $y * $stride, $w * 4) }   # XOR (BGRA, bottom-up)
    for ($y = $h - 1; $y -ge 0; $y--) {                                          # AND mask from alpha
        $row = New-Object byte[] $maskRow
        for ($x = 0; $x -lt $w; $x++) {
            if ($px[$y * $stride + $x * 4 + 3] -lt 128) {
                $row[[int][math]::Floor($x / 8)] = $row[[int][math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8))
            }
        }
        $bw.Write($row)
    }
    $bw.Flush()
    return $ms.ToArray()
}

# --- build frames ---
$frames = @()
foreach ($s in @(16, 20, 24, 32, 48, 64)) {
    $bmp = Draw-Icon $s
    $frames += , @($s, (Get-BmpFrame $bmp))
    $bmp.Dispose()
}
$bmp256 = Draw-Icon 256
$msP = New-Object System.IO.MemoryStream
$bmp256.Save($msP, [System.Drawing.Imaging.ImageFormat]::Png)
$frames += , @(256, $msP.ToArray())

# --- previews ---
Save-Preview (Draw-Icon 256) 1  (Join-Path $PreviewDir 'preview_256.png')
Save-Preview (Draw-Icon 32)  8  (Join-Path $PreviewDir 'preview_32_x8.png')
Save-Preview (Draw-Icon 16)  10 (Join-Path $PreviewDir 'preview_16_x10.png')

# --- write ICO ---
$fs = [System.IO.File]::Create($IcoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([int16]0); $bw.Write([int16]1); $bw.Write([int16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($fr in $frames) {
    $s = $fr[0]; $data = $fr[1]
    $bw.Write([byte]($s -band 0xFF)); $bw.Write([byte]($s -band 0xFF))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([int16]1); $bw.Write([int16]32)
    $bw.Write([int]$data.Length); $bw.Write([int]$offset)
    $offset += $data.Length
}
foreach ($fr in $frames) { $bw.Write([byte[]]$fr[1]) }
$bw.Flush(); $fs.Close()

Write-Output "ICO written: $IcoPath ($((Get-Item $IcoPath).Length) bytes, $($frames.Count) frames)"
Write-Output "Previews:    $PreviewDir"
