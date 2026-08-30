# Sets one or more `key = value` lines in the running dock's settings file.
# Rewrites the file in place so the config watcher sees a plain write, not the
# rename an atomic save would do.
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Pairs)
$path = Join-Path $env:LOCALAPPDATA 'LiquiDock\settings.txt'
$lines = [System.IO.File]::ReadAllLines($path)
foreach ($pair in $Pairs) {
    $k,$v = $pair -split '=',2
    $k = $k.Trim(); $v = $v.Trim()
    $found = $false
    for ($i=0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match "^\s*$([regex]::Escape($k))\s*=") { $lines[$i] = "$k = $v"; $found = $true }
    }
    if (-not $found) { $lines += "$k = $v" }
}
[System.IO.File]::WriteAllLines($path, $lines, (New-Object System.Text.UTF8Encoding($true)))
Write-Output "set: $($Pairs -join ' ')"
