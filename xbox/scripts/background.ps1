#requires -Version 7.2
# The launcher's background picture: converts xbox/extras/fundo.png into Assets/fundo.rgba of the package layout.
# Format (read by SpartanOverlay::SetBackground): u32 width, u32 height (little endian), then width*height*4 bytes B,G,R,A.
# Without xbox/extras/fundo.png the launcher keeps its black background. Pictures wider than 1920 are scaled down.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$root=Split-Path $PSScriptRoot -Parent
$source=Join-Path $root 'extras/fundo.png'
$assets=Join-Path $root '.local/build/layout/Assets'
New-Item -ItemType Directory -Force $assets | Out-Null
$target=Join-Path $assets 'fundo.rgba'
if(!(Test-Path -LiteralPath $source)) {
    Remove-Item -LiteralPath $target -ErrorAction SilentlyContinue
    Write-Output 'No launcher background (xbox/extras/fundo.png is missing).'
    return
}
$image=[Drawing.Image]::FromFile($source)
try {
    $scale=[Math]::Min(1.0,1920.0/$image.Width)
    $width=[int][Math]::Round($image.Width*$scale)
    $height=[int][Math]::Round($image.Height*$scale)
    $bitmap=[Drawing.Bitmap]::new($width,$height,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.Clear([Drawing.Color]::Black)
            $graphics.DrawImage($image,0,0,$width,$height)
        } finally { $graphics.Dispose() }
        $data=$bitmap.LockBits([Drawing.Rectangle]::new(0,0,$width,$height),[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            if($data.Stride -ne $width*4) { throw "Unexpected stride $($data.Stride)" }
            $pixels=[byte[]]::new($width*$height*4)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0,$pixels,0,$pixels.Length)
        } finally { $bitmap.UnlockBits($data) }
    } finally { $bitmap.Dispose() }
} finally { $image.Dispose() }
$stream=[IO.File]::Create($target)
try {
    $stream.Write([BitConverter]::GetBytes([uint32]$width),0,4)
    $stream.Write([BitConverter]::GetBytes([uint32]$height),0,4)
    $stream.Write($pixels,0,$pixels.Length)
} finally { $stream.Dispose() }
Write-Output "Launcher background: ${width}x${height} written to Assets/fundo.rgba."