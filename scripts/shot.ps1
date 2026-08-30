# Grabs a rectangle of the screen to a PNG, optionally parking the cursor first.
#
# The dock is verified by measuring its render, not by looking at it: its fill is
# a few percent white over whatever is behind it, so a correct frame and a failed
# one look much the same. This is what feeds those measurements.
#
#   .\scripts\shot.ps1 -Out out.png -X 508 -Y 1355 -W 1544 -H 85
#
# Note that `backdrop = screen` sets WDA_EXCLUDEFROMCAPTURE on the dock, which
# makes it invisible here as well as in the user's own screenshots. Switch to
# `backdrop = wallpaper` before capturing anything.
param([string]$Out, [int]$X=373, [int]$Y=1245, [int]$W=1814, [int]$H=195, [int]$CurX=-1, [int]$CurY=-1, [int]$SettleMs=900)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if ($CurX -ge 0) { [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point($CurX,$CurY) }
Start-Sleep -Milliseconds $SettleMs
$bmp = New-Object System.Drawing.Bitmap($W,$H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X,$Y,0,0,(New-Object System.Drawing.Size($W,$H)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out"
