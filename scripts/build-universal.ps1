<#
.SYNOPSIS
    One PC build for every release set up so far: the launcher plus
    mhfu_host_eu.exe, mhfu_host_us.exe, mhfu_host_jp.exe, mhfu_host_fuc.exe.

.DESCRIPTION
    Each release (Europe, USA, Japan, FUComplete) is recompiled from its own
    EBOOT, so each is a game of its own; setup.ps1 makes each one's code (in
    work\, work\us, work\jp, work\fuc). This builds all of them in one build
    folder (build-all\Release), next to one launcher that starts the right one
    for the player's disc (launcher/regions.h). A release not set up yet is
    left out.

.EXAMPLE
    .\scripts\build-universal.ps1
#>
[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$BuildDir = ""
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-all" }

$targets = @("mhfu_launcher")
foreach ($r in @(@("eu", "work"), @("us", "work\us"), @("jp", "work\jp"), @("fuc", "work\fuc"))) {
    if (Get-ChildItem (Join-Path $repo "$($r[1])\gen") -Filter *.c -ErrorAction SilentlyContinue | Select-Object -First 1) {
        $targets += "mhfu_host_$($r[0])"
    } else {
        Write-Host "  $($r[0]): not set up ($($r[1])\gen is empty), left out"
    }
}
Write-Host "Building: $($targets -join ', ')" -ForegroundColor Cyan
cmake -S $repo -B $BuildDir -DMHFU_UNIVERSAL=ON
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $BuildDir --config $Config --target @targets
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "`nDone: $BuildDir\$Config\mhfu_launcher.exe" -ForegroundColor Green
