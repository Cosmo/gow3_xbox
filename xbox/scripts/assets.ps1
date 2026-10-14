#requires -Version 7.2
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$root=Split-Path $PSScriptRoot -Parent
$assets=Join-Path $root '.local/build/layout/Assets'
New-Item -ItemType Directory -Force $assets | Out-Null
foreach($item in @(@('StoreLogo.png',50,50),@('Square150x150Logo.png',150,150),@('Square44x44Logo.png',44,44),@('SplashScreen.png',620,300))) {
    $bitmap=[Drawing.Bitmap]::new($item[1],$item[2])
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $font=[Drawing.Font]::new('Segoe UI',[float]([Math]::Min($item[1],$item[2])*0.45),[Drawing.FontStyle]::Bold)
    try {
        $graphics.Clear([Drawing.Color]::FromArgb(24,24,24))
        $format=[Drawing.StringFormat]::new(); $format.Alignment='Center'; $format.LineAlignment='Center'
        $graphics.DrawString('III',$font,[Drawing.Brushes]::Firebrick,[Drawing.RectangleF]::new(0,0,$item[1],$item[2]),$format)
        $format.Dispose()
        $bitmap.Save((Join-Path $assets $item[0]),[Drawing.Imaging.ImageFormat]::Png)
    } finally { $font.Dispose(); $graphics.Dispose(); $bitmap.Dispose() }
}
