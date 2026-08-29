<#
.SYNOPSIS
    Configures and builds LiquiDock.

.DESCRIPTION
    Locates Visual Studio, enters its developer environment (which puts MSVC,
    the Windows SDK, CMake and Ninja on PATH), then configures and builds the
    requested preset. Works from any shell, so nobody has to remember to open a
    "Developer PowerShell" first.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Preset release -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'debug',

    # Launch the dock once the build succeeds.
    [switch]$Run,

    # Delete the preset's build directory first.
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio Installer not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.'
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $vsPath) {
    Write-Host ''
    Write-Host 'The C++ toolchain is missing.' -ForegroundColor Red
    Write-Host 'Visual Studio is installed, but without the "Desktop development with C++" workload.'
    Write-Host 'Install it (as administrator):'
    Write-Host ''
    Write-Host '  & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vs_installer.exe" modify ``'
    Write-Host '      --installPath "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community" ``'
    Write-Host '      --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive'
    Write-Host ''
    throw 'Missing MSVC toolset.'
}

$devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Set-Location $repoRoot

$buildDir = Join-Path $repoRoot "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# A running dock holds a write lock on its own exe, and the linker fails with
# LNK1168 rather than anything that names the cause. Stop it before building.
$running = Get-Process liquidock -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "Stopping running LiquiDock instance before linking..." -ForegroundColor Yellow
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

# Windows PowerShell turns any stderr line from a native exe into a terminating
# NativeCommandError while ErrorActionPreference is 'Stop', so a harmless CMake
# warning would abort a build that actually succeeded. The exit code is the real
# gate.
$ErrorActionPreference = 'Continue'

cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = 'Stop'; throw "Configure failed." }

cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { $ErrorActionPreference = 'Stop'; throw "Build failed." }

$ErrorActionPreference = 'Stop'

$exe = Join-Path $buildDir 'liquidock.exe'
Write-Host ''
Write-Host "Built $exe" -ForegroundColor Green

if ($Run) {
    # Stop a previous instance first; the single-instance guard would otherwise
    # make the new build exit immediately and silently.
    Get-Process liquidock -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Process $exe
}
