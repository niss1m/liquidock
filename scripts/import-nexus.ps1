<#
.SYNOPSIS
Imports a Winstep Nexus dock into LiquiDock.

.DESCRIPTION
Nexus keeps everything in the registry, under HKCU\Software\WinSTEP2000\NeXuS.
Its items live in the Docks subkey as flat values named <dock><field><index>:

    DockNoItems1 = 49            how many items dock 1 has
    1Path4       = ...\Photoshop.exe
    1IconPath4   = ...\photoshop.png
    1StartPath4  = ...\Adobe Photoshop 2024\
    1Label4      = Photoshop
    1Arg4        = --flag
    1Type4       = 0             0 is an ordinary shortcut; other values are
                                 Nexus's own modules - clock, weather, recycler -
                                 which have no equivalent here and are skipped.

This reads that and writes LiquiDock's items.txt, keeping each entry's custom
icon, arguments and working directory. It never writes to the Nexus keys.

.PARAMETER Dock
Which Nexus dock to import. Defaults to 1, the main one.

.PARAMETER Utility
Names or paths matched against the tail of the dock; anything matching lands in
the utility run to the right of the hairline. Defaults to the recycle bin.

.PARAMETER OutFile
Where to write. Defaults to LiquiDock's own items.txt.

.PARAMETER WhatIf
Print what would be imported without writing anything.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [int]$Dock = 1,
    [string[]]$Utility = @('recycle', 'trash', 'downloads'),
    [string]$OutFile = (Join-Path $env:LOCALAPPDATA 'LiquiDock\items.txt')
)

$ErrorActionPreference = 'Stop'

$key = 'HKCU:\Software\WinSTEP2000\NeXuS\Docks'
if (-not (Test-Path $key)) {
    Write-Error "Nexus configuration not found at $key. Is Winstep Nexus installed for this user?"
    return
}

$values = Get-ItemProperty $key
$count = $values."DockNoItems$Dock"
if (-not $count) {
    Write-Error "Dock $Dock has no items (DockNoItems$Dock is missing). Available docks: $($values.NoDocks)"
    return
}

Write-Host ("Nexus dock {0} '{1}' reports {2} items." -f $Dock, $values."DockName$Dock", $count)

function Field([string]$name, [int]$index) {
    $v = $values."$Dock$name$index"
    if ($null -eq $v) { return '' }
    return [string]$v
}

# Store apps are looked up by name; asking once is much cheaper than per item.
$startApps = @()
try { $startApps = Get-StartApps -ErrorAction Stop } catch {
    Write-Host 'Get-StartApps unavailable; Store apps will be skipped.'
}

$items = @()
$skipped = @()

for ($i = 0; $i -lt [int]$count; $i++) {
    $path = Field 'Path' $i
    $type = Field 'Type' $i
    $label = Field 'Label' $i
    $icon = Field 'IconPath' $i
    $args = Field 'Arg' $i
    $workdir = Field 'StartPath' $i

    # Nexus stores two different things this way. Its own modules - the clock,
    # the recycler, the weather applet - are a bare "*<n>" with no target, and
    # there is nothing to point at. But Store apps are *also* pathless: Nexus
    # keeps them as a shell PIDL, which is not readable from PowerShell.
    #
    # Those can still be recovered, because a Store app that is on someone's
    # dock is on their Start menu too, and Get-StartApps gives the AppID that
    # `shell:AppsFolder\<id>` needs. So a pathless entry is matched by name
    # first, and only reported as skipped when that fails as well.
    if ([string]::IsNullOrWhiteSpace($path) -or $path -match '^\*\d+$') {
        $resolved = ''
        if ($label) {
            $app = $startApps | Where-Object { $_.Name -eq $label } | Select-Object -First 1
            if (-not $app) {
                $app = $startApps | Where-Object { $_.Name -like "$label*" } | Select-Object -First 1
            }
            if ($app) { $resolved = 'shell:AppsFolder\' + $app.AppID }
        }
        if (-not $resolved) {
            $what = if ($label) { $label } else { "module $path" }
            $skipped += $what
            continue
        }
        $path = $resolved
        $workdir = ''
    }

    if (-not $label) { $label = [IO.Path]::GetFileNameWithoutExtension($path) }

    $group = 'main'
    foreach ($pattern in $Utility) {
        if ($label -match $pattern -or $path -match $pattern) { $group = 'utility'; break }
    }

    # Only keep an icon that is actually there; a stale path would silently fall
    # back to the shell icon at runtime, which is fine but worth not writing.
    if ($icon -and -not (Test-Path -LiteralPath $icon)) {
        Write-Host ("  icon missing, using the shell's: {0}" -f $icon)
        $icon = ''
    }

    $items += [pscustomobject]@{
        Group   = $group
        Path    = $path
        Label   = $label
        Args    = $args
        WorkDir = $workdir.TrimEnd('\')
        Icon    = $icon
    }
}

# LiquiDock's own ceiling (design::kMaxItems), mirrored here so the truncation
# is explained rather than happening silently when the dock loads the file.
$limit = 64
if ($items.Count -gt $limit) {
    Write-Host ("  {0} items is over LiquiDock's limit of {1}; keeping the first {1}." -f $items.Count, $limit)
    $items = $items | Select-Object -First $limit
}

Write-Host ''
Write-Host ("Importing {0} items:" -f $items.Count)
foreach ($item in $items) {
    $marks = @()
    if ($item.Icon)    { $marks += 'icon' }
    if ($item.Args)    { $marks += 'args' }
    if ($item.WorkDir) { $marks += 'workdir' }
    $suffix = if ($marks) { '  [' + ($marks -join ', ') + ']' } else { '' }
    Write-Host ("  {0,-8} {1}{2}" -f $item.Group, $item.Label, $suffix)
}
if ($skipped.Count) {
    Write-Host ''
    Write-Host ("Skipped {0} Nexus module(s) with no LiquiDock equivalent: {1}" -f `
        $skipped.Count, ($skipped -join ', '))
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# Imported from Winstep Nexus by scripts\import-nexus.ps1')
$lines.Add(('# {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm')))
$lines.Add('')
foreach ($item in $items) {
    $lines.Add('[item]')
    $lines.Add('group   = ' + $item.Group)
    $lines.Add('path    = ' + $item.Path)
    $lines.Add('label   = ' + $item.Label)
    if ($item.Args)    { $lines.Add('args    = ' + $item.Args) }
    if ($item.WorkDir) { $lines.Add('workdir = ' + $item.WorkDir) }
    if ($item.Icon)    { $lines.Add('icon    = ' + $item.Icon) }
    $lines.Add('')
}

if ($PSCmdlet.ShouldProcess($OutFile, 'write LiquiDock items')) {
    if (Test-Path -LiteralPath $OutFile) {
        $backup = "$OutFile.bak"
        Copy-Item -LiteralPath $OutFile -Destination $backup -Force
        Write-Host ''
        Write-Host ("Previous items backed up to {0}" -f $backup)
    }
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $lines | Set-Content -LiteralPath $OutFile -Encoding utf8
    Write-Host ("Wrote {0}" -f $OutFile)
    Write-Host 'Restart LiquiDock, or it will pick the change up on its own if it is watching.'
}
