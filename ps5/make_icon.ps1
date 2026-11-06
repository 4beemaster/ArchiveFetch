$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bitmap = [System.Drawing.Bitmap]::new(512, 512)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$background = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(24, 53, 43))
$accent = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(107, 221, 165))
$pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 14)
$font = [System.Drawing.Font]::new('Segoe UI', 34, [System.Drawing.FontStyle]::Bold)
$format = [System.Drawing.StringFormat]::new()
$format.Alignment = [System.Drawing.StringAlignment]::Center
try {
    $graphics.FillRectangle($background, 0, 0, 512, 512)
    $graphics.FillRectangle($accent, 78, 96, 356, 66)
    $graphics.DrawRectangle($pen, 98, 182, 316, 200)
    $graphics.DrawLine($pen, 256, 212, 256, 326)
    $graphics.DrawLine($pen, 212, 280, 256, 326)
    $graphics.DrawLine($pen, 300, 280, 256, 326)
    $graphics.DrawString('ArchiveFetch', $font, [System.Drawing.Brushes]::White, [System.Drawing.RectangleF]::new(0, 418, 512, 70), $format)
    $bitmap.Save((Join-Path $PSScriptRoot 'icon0.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $format.Dispose()
    $font.Dispose()
    $pen.Dispose()
    $accent.Dispose()
    $background.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}