<#
.SYNOPSIS
    One PC build for every release set up so far: the launcher plus
    mhfu_host_eu.exe, mhfu_host_us.exe, mhfu_host_jp.exe, mhfu_host_fuc.exe.

.DESCRIPTION
    Each release (Europe, USA, Japan, FUComplete) is recompiled from its own
    EBOOT, so each is a game of its own; setup.ps1 makes each one's code (in
    work\, work\us, work\jp, work\fuc). This builds all of them in one build
    folder, next to one launcher that starts the right one for the player's
    disc (launcher/regions.h). A release not set up yet is left out.

    With Ninja (the default) the build runs MSVC through ccache: every object
    is kept by its contents (%LOCALAPPDATA%\ccache), so after setup.ps1
    re-emits the C, or in a fresh build folder, only what really changed
    compiles. The executables land in <BuildDir>\Release as before.
    -Generator VS builds the old way (Visual Studio solution, no cache) in build-all.

    Needs ccache and ninja (winget install Ccache.Ccache Ninja-build.Ninja);
    without ccache it still builds, uncached.

.EXAMPLE
    .\scripts\build-universal.ps1
#>
[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$BuildDir = "",
    [ValidateSet("Ninja", "VS")] [string]$Generator = "Ninja"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo $(if ($Generator -eq "Ninja") { "build-all-ninja" } else { "build-all" }) }

$targets = @("mhfu_launcher")
foreach ($r in @(@("eu", "work"), @("us", "work\us"), @("jp", "work\jp"), @("fuc", "work\fuc"))) {
    if (Get-ChildItem (Join-Path $repo "$($r[1])\gen") -Filter *.c -ErrorAction SilentlyContinue | Select-Object -First 1) {
        $targets += "mhfu_host_$($r[0])"
    } else {
        Write-Host "  $($r[0]): not set up ($($r[1])\gen is empty), left out"
    }
}
Write-Host "Building: $($targets -join ', ')" -ForegroundColor Cyan

. "$PSScriptRoot\ninja-msvc.ps1"
$cmakeArgs = @("-DMHFU_UNIVERSAL=ON", "-DMHFU_TRACE=LITE")
if ($Generator -eq "Ninja") { $cmakeArgs += Get-NinjaMsvcArgs -BuildDir $BuildDir -Config $Config }

# CMake's warnings go to stderr, which Windows PowerShell takes for an error
# under 'Stop': relaxed while it runs, the exit code decides.
$ErrorActionPreference = 'Continue'
$t0 = Get-Date
cmake -S $repo -B $BuildDir @cmakeArgs
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $BuildDir --config $Config --target @targets
if ($LASTEXITCODE) { throw "build failed" }
$ErrorActionPreference = 'Stop'
$min = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
$ccache = Find-WingetTool "ccache" "Ccache.Ccache"
if ($Generator -eq "Ninja" -and $ccache) { & $ccache --show-stats | Select-String "Hits|Misses" | ForEach-Object { "  $_" } }
Write-Host "`nDone in $min min: $BuildDir\$Config\mhfu_launcher.exe" -ForegroundColor Green
