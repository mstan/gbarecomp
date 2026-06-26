# tcp_screenshot.ps1 — capture a frame from a running gbarecomp `--tcp` instance
# and decode the framebuffer to a PNG you can view.
#
# The runtime's TCP debug server (see TCP.md) answers `{"cmd":"screenshot"}`
# with {ok,w,h,data:<hex RGB framebuffer>}; this fetches it, decodes the hex,
# and writes a (nearest-neighbour upscaled) PNG. Works while the core free-runs.
#
# Usage:
#   .\tools\tcp_screenshot.ps1 [-Port 19842] [-Out frame.png] [-Scale 3] [-Continue]
#   -Continue  : send `continue` first (a --tcp instance starts PAUSED).

param(
    [int]$Port      = 19842,
    [string]$Out    = "frame.png",
    [int]$Scale     = 3,
    [switch]$Continue
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function Send-DbgCmd([string]$cmd, [int]$timeoutMs = 20000) {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $Port)
    $c.ReceiveTimeout = $timeoutMs
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s); $w.NewLine = "`n"; $w.AutoFlush = $true
    $w.WriteLine($cmd)
    $line = (New-Object IO.StreamReader($s)).ReadLine()
    $c.Close()
    return $line
}

if ($Continue) { Send-DbgCmd '{"cmd":"continue"}' | Out-Null; Start-Sleep -Milliseconds 500 }

$j = Send-DbgCmd '{"cmd":"screenshot"}' | ConvertFrom-Json
if (-not $j.ok) { throw "screenshot failed: $($j.error)" }
$w = [int]$j.w; $h = [int]$j.h; $hex = $j.data
$n = $hex.Length / 2
$raw = New-Object byte[] $n
for ($i = 0; $i -lt $n; $i++) { $raw[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
$bpp = [int]($n / ($w * $h))   # 3 (RGB) or 4 (RGBA)

# 24bppRgb is BGR in memory; the framebuffer is RGB → swap R/B per pixel.
$bgr = New-Object byte[] ($w * $h * 3)
for ($p = 0; $p -lt ($w * $h); $p++) {
    $s = $p * $bpp; $d = $p * 3
    $bgr[$d] = $raw[$s + 2]; $bgr[$d + 1] = $raw[$s + 1]; $bgr[$d + 2] = $raw[$s]
}
$bmp  = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$bd   = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
for ($y = 0; $y -lt $h; $y++) {
    [System.Runtime.InteropServices.Marshal]::Copy($bgr, $y * $w * 3, [IntPtr]::Add($bd.Scan0, $y * $bd.Stride), $w * 3)
}
$bmp.UnlockBits($bd)

if ($Scale -gt 1) {
    $big = New-Object System.Drawing.Bitmap(($w * $Scale), ($h * $Scale))
    $gx  = [System.Drawing.Graphics]::FromImage($big)
    $gx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $gx.DrawImage($bmp, 0, 0, $w * $Scale, $h * $Scale)
    $big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $big.Dispose()
} else {
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
}
$bmp.Dispose()
Write-Host "saved $Out  (${w}x${h} x$Scale, frame=$($j.frame))"
