<#
.SYNOPSIS
    The Linux / Steam Deck build, from Windows: runs scripts/build-linux.sh in WSL.

.DESCRIPTION
    Uses the code setup.ps1 already recompiled (work\gen, work\<region>\...);
    nothing is set up again. The build folder is inside WSL
    (~/.cache/mhfu/build-linux[-universal]) and ccache keeps the objects, so a
    rebuild only compiles what changed. The first run of a fresh WSL needs the
    packages in docs/LINUX.md.

.EXAMPLE
    .\scripts\build-linux.ps1 -Universal -Package
#>
[CmdletBinding()]
param(
    [switch]$Universal,
    [switch]$Package,
    [string]$Distro = "Ubuntu-24.04"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$wslRepo = (& wsl -d $Distro --exec wslpath -a ($repo -replace '\\', '/')).Trim()
$opts = @()
if ($Universal) { $opts += "--universal" }
if ($Package) { $opts += "--package" }
& wsl -d $Distro --cd $wslRepo --exec bash scripts/build-linux.sh @opts
if ($LASTEXITCODE) { throw "Linux build failed" }
