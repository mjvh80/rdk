param(
    [string]$Output = "$PSScriptRoot/../assets/rdk.ico",
    [string]$Preview = "$PSScriptRoot/../build/rdk-icon.png"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$frames = foreach ($size in @(16, 20, 24, 32, 40, 48, 60, 64, 80, 96, 128, 256)) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size / 64.0, $size / 64.0)
    $ink = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#2563EB'))
    $accent = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#FFE14D'))
    $outline = [System.Drawing.Pen]::new($ink.Color, 5)
    $arrowOutline = [System.Drawing.Pen]::new($ink.Color, 1.5)
    $arrowOutline.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.DrawRectangle($outline, 8, 19, 41, 28)
    $graphics.FillRectangle($ink, 25, 48, 7, 6)
    $graphics.FillRectangle($ink, 16, 54, 25, 4)
    $clear = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::Transparent)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.FillRectangle($clear, 32, 7, 25, 27)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $arrow = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(36, 10), [System.Drawing.PointF]::new(54, 10),
        [System.Drawing.PointF]::new(54, 28), [System.Drawing.PointF]::new(48, 28),
        [System.Drawing.PointF]::new(48, 20), [System.Drawing.PointF]::new(35, 33),
        [System.Drawing.PointF]::new(31, 29), [System.Drawing.PointF]::new(44, 16),
        [System.Drawing.PointF]::new(36, 16)
    )
    $graphics.FillPolygon($accent, $arrow)
    $graphics.DrawPolygon($arrowOutline, $arrow)
    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($size -eq 256) {
        [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($Preview))) | Out-Null
        $bitmap.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    [PSCustomObject]@{ Size = $size; Bytes = $stream.ToArray() }
    $stream.Dispose()
    $outline.Dispose()
    $arrowOutline.Dispose()
    $clear.Dispose()
    $accent.Dispose()
    $ink.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($Output))) | Out-Null
$file = [System.IO.File]::Create($Output)
$writer = [System.IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
} finally {
    $writer.Dispose()
}
Write-Output "Generated $Output with $($frames.Count) PNG icon sizes (16-256 px)."