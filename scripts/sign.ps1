# Signs a built LiquiDock binary.
#
# Code signing needs a certificate issued to a verified identity. There is no
# way around that and nothing here creates one: an unsigned binary and a binary
# signed by a certificate nobody trusts both say "Unknown Publisher", so a
# self-signed signature buys nothing except the appearance of one.
#
# The three routes that do work, cheapest first:
#
#   Azure Trusted Signing  ~$10/month, Microsoft's own service. The certificate
#                          lives in Azure and signtool talks to it through a
#                          dlib; no token to keep in a drawer. Requires a
#                          verified organisation or, for individuals, three
#                          years of verifiable history.
#   OV certificate         ~$200-400/year from a CA. Signs fine, but SmartScreen
#                          still warns until the binary builds reputation - a
#                          few hundred installs, or a few weeks.
#   EV certificate         ~$300-600/year, on a hardware token or cloud HSM.
#     the only one          Carries SmartScreen reputation from the first
#     with instant trust    signature, which is why shipping software buys it.
#
# Once you have one, this script wants either its thumbprint (if it is in your
# certificate store) or a .pfx and its password:
#
#   .\scripts\sign.ps1 -Thumbprint A1B2C3...
#   .\scripts\sign.ps1 -PfxPath codesign.pfx -PfxPassword (Read-Host -AsSecureString)
#
# Timestamping is not optional. Without it the signature dies with the
# certificate, and every copy already shipped stops verifying on that date.

param(
    [string]$Exe = "$PSScriptRoot\..\build\release\liquidock.exe",
    [string]$Thumbprint,
    [string]$PfxPath,
    [System.Security.SecureString]$PfxPassword,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) {
    Write-Error "Nothing to sign at $Exe - build it first."
}

# The newest SDK build tools on the machine. signtool moves between versions and
# hard-coding one is how a build script rots.
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object { $_.Directory.Parent.Name } -Descending |
    Select-Object -First 1
if (-not $signtool) {
    Write-Error 'signtool.exe not found. Install the Windows SDK.'
}

$args = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', $TimestampUrl, '/v')

if ($Thumbprint) {
    $args += @('/sha1', $Thumbprint)
} elseif ($PfxPath) {
    if (-not (Test-Path $PfxPath)) { Write-Error "No .pfx at $PfxPath" }
    $args += @('/f', $PfxPath)
    if ($PfxPassword) {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($PfxPassword))
        $args += @('/p', $plain)
    }
} else {
    Write-Host 'No certificate given, so nothing was signed.' -ForegroundColor Yellow
    Write-Host 'Pass -Thumbprint or -PfxPath. See the notes at the top of this file.'
    exit 1
}

$args += $Exe
& $signtool.FullName @args
if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed with $LASTEXITCODE" }

Write-Host ''
& $signtool.FullName verify /pa /v $Exe
