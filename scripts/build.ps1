<#
.SYNOPSIS
    Builds targets in an existing build folder, Ninja or Visual Studio alike.

.DESCRIPTION
    A Ninja folder (setup.ps1's and build-universal.ps1's default) needs MSVC's
    environment for every build; this imports it first (scripts\ninja-msvc.ps1),
    so no Developer Prompt is needed. Executables land in <BuildDir>\Release.

.EXAMPLE
    .\scripts\build.ps1                                  # build\: mhfu_host
    .\scripts\build.ps1 -BuildDir build-us -Target mhfu_host,mhfu_launcher
    .\scripts\build.ps1 -BuildDir build-all-ninja -Target mhfu_host_eu
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string[]]$Target = @("mhfu_host"),
    [string]$Config = "Release"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repo $BuildDir }
if (-not (Test-Path "$BuildDir\CMakeCache.txt")) { throw "$BuildDir is not configured (run setup.ps1 or build-universal.ps1)" }
. "$PSScriptRoot\ninja-msvc.ps1"
if (Select-String -Path "$BuildDir\CMakeCache.txt" -Pattern '^CMAKE_GENERATOR:INTERNAL=Ninja' -Quiet) { Import-MsvcEnv }
$ErrorActionPreference = 'Continue'      # CMake's warnings go to stderr
cmake --build $BuildDir --config $Config --target @Target
exit $LASTEXITCODE
