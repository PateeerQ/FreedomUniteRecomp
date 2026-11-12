# Dot-sourced by setup.ps1 and build-universal.ps1: a CMake build with Ninja,
# MSVC and ccache. CMake's Visual Studio generators run no compiler launcher,
# so ccache needs Ninja; ccache keeps every object by its contents
# (%LOCALAPPDATA%\ccache), so after setup re-emits the recompiled C (new
# timestamps, mostly the same code) only what changed compiles again.
#
#   . "$PSScriptRoot\ninja-msvc.ps1"
#   $args = Get-NinjaMsvcArgs -BuildDir $build -Config Release
#   cmake -S $repo -B $build @args

# A tool on PATH, else where winget put it (its Links folder is not always made).
function Find-WingetTool([string]$name, [string]$package) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $f = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\$package*" -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($f) { return $f.FullName }
    return $null
}

# MSVC's environment (cl, link, INCLUDE, LIB), as the Developer Command Prompt
# has it, imported into this PowerShell once. A Ninja build folder needs it for
# every `cmake --build` (the VS solution finds it by itself).
function Import-MsvcEnv {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "no vcvars64.bat in $vs (Visual Studio's C++ tools)" }
    # (vcvars complains on stderr when vswhere is not on PATH; harmless, but 'Stop' would end us)
    cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

# Whether a Ninja build can be made here (ninja and the C++ tools found).
function Test-NinjaMsvc {
    $ninja = Find-WingetTool "ninja" "Ninja-build.Ninja"
    if (-not $ninja) { $ninja = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\cmake\*\bin\ninja.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 }
    return [bool]$ninja
}

# The MSVC environment imported (once), and the cmake arguments: Ninja, the
# config, ccache when installed, and every executable in <BuildDir>\<Config>
# (where the Visual Studio builds put them, so run.ps1 and the rest find them).
function Get-NinjaMsvcArgs([string]$BuildDir, [string]$Config = "Release") {
    $ninja = Find-WingetTool "ninja" "Ninja-build.Ninja"
    if (-not $ninja) { $ninja = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\cmake\*\bin\ninja.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 -ExpandProperty FullName }
    if (-not $ninja) { throw "no ninja: winget install Ninja-build.Ninja (or -Generator VS)" }
    $ccache = Find-WingetTool "ccache" "Ccache.Ccache"
    if ($ccache) {
        & $ccache --max-size 40G | Out-Null      # four releases' objects, plus room to spare
        Write-Host "ccache: $ccache" -ForegroundColor DarkGray
    } else {
        Write-Host "no ccache (winget install Ccache.Ccache): building without the cache" -ForegroundColor Yellow
    }
    Import-MsvcEnv
    $a = @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Config",
           "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
           "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$($BuildDir -replace '\\', '/')/$Config")
    if ($ccache) { $a += @("-DCMAKE_C_COMPILER_LAUNCHER=$ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache") }
    return $a
}

# A build folder made by a Visual Studio generator cannot be switched to Ninja:
# it is renamed out of the way (<dir>-vs, kept, never deleted), and the cache
# values worth keeping (MHFU_TRACE: build\ is FULL for bring-up) come back as
# -D arguments.
function Move-VsBuildAside([string]$BuildDir) {
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path $cache)) { return @() }
    if (-not (Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=Visual Studio' -Quiet)) { return @() }
    $keep = @()
    foreach ($name in @("MHFU_TRACE", "MHFU_RELAY", "MHFU_RCHEEVOS", "MHFU_TEXPACK")) {
        $m = Select-String -Path $cache -Pattern "^${name}:[A-Z]+=(.*)$" | Select-Object -First 1
        if ($m -and $m.Matches[0].Groups[1].Value -ne "") { $keep += "-D$name=$($m.Matches[0].Groups[1].Value)" }
    }
    $aside = "$BuildDir-vs"
    if (Test-Path $aside) { $aside = "$BuildDir-vs-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    Rename-Item -LiteralPath $BuildDir -NewName (Split-Path -Leaf $aside)
    Write-Host "  $BuildDir was a Visual Studio build: kept as $aside, Ninja builds in its place ($($keep -join ' '))" -ForegroundColor Yellow
    return $keep
}
