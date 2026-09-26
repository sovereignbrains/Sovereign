<#
.SYNOPSIS
  Builds assets/sovereign.ico from a square PNG with transparency.

.DESCRIPTION
  One PNG-compressed entry per size (Windows reads PNG entries in .ico since
  Vista): 16, 20, 24, 32, 40, 48, 64, 256 - the small ones cover the tray at
  100-250% scaling, 256 the flyout header and Explorer. Resized with bicubic
  sampling on a transparent canvas. Run it again when the artwork changes; the
  .ico is committed, the build doesn't depend on this script.

.EXAMPLE
  pwsh tools/icon/make_ico.ps1 -Png art.png
#>
param(
  [Parameter(Mandatory)][string]$Png,
  [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'assets/sovereign.ico')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$source = [System.Drawing.Image]::FromFile((Resolve-Path $Png).Path)
try {
  $entries = foreach ($size in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($source, 0, 0, $size, $size)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    [pscustomobject]@{ Size = $size; Data = $ms.ToArray() }
  }
} finally { $source.Dispose() }

# ICONDIR, then one ICONDIRENTRY per image, then the PNG blobs.
$stream = [System.IO.MemoryStream]::new()
$w = [System.IO.BinaryWriter]::new($stream)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
  $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
  $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32)
  $w.Write([uint32]$e.Data.Length); $w.Write([uint32]$offset)
  $offset += $e.Data.Length
}
foreach ($e in $entries) { $w.Write($e.Data) }
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $stream.ToArray())
"$Out : $($entries.Count) sizes, $($stream.Length) bytes"
