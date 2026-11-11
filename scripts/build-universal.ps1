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

# A tool on PATH, else where winget put it (its Links folder is not always made).
function Find-Tool([string]$name, [string]$package) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $f = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\$package*" -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($f) { return $f.FullName }
    return $null
}

$cmakeArgs = @("-DMHFU_UNIVERSAL=ON", "-DMHFU_TRACE=LITE")
if ($Generator -eq "Ninja") {
    $ninja = Find-Tool "ninja" "Ninja-build.Ninja"
    if (-not $ninja) { $ninja = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\cmake\*\bin\ninja.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 -ExpandProperty FullName }
    if (-not $ninja) { throw "no ninja: winget install Ninja-build.Ninja (or -Generator VS)" }
    $ccache = Find-Tool "ccache" "Ccache.Ccache"
    if ($ccache) {
        & $ccache --max-size 40G | Out-Null      # four releases' objects, plus room to spare
        Write-Host "ccache: $ccache" -ForegroundColor DarkGray
    } else {
        Write-Host "no ccache (winget install Ccache.Ccache): building without the cache" -ForegroundColor Yellow
    }
    # MSVC's environment (cl, link, the SDK), as the Developer Command Prompt has it.
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
        if (-not (Test-Path $vcvars)) { throw "no vcvars64.bat in $vs (Visual Studio's C++ tools)" }
        # (vcvars complains on stderr when vswhere is not on PATH; harmless, but 'Stop' would end us)
        cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
    }
    $cmakeArgs += @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Config",
                    "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
                    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$($BuildDir -replace '\\', '/')/$Config")
    if ($ccache) { $cmakeArgs += @("-DCMAKE_C_COMPILER_LAUNCHER=$ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache") }
}

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
if ($Generator -eq "Ninja" -and $ccache) { & $ccache --show-stats | Select-String "Hits|Misses" | ForEach-Object { "  $_" } }
Write-Host "`nDone in $min min: $BuildDir\$Config\mhfu_launcher.exe" -ForegroundColor Green
