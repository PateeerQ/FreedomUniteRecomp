<#
.SYNOPSIS
    Prepare a recompiled MHFU build from your own dump.

.DESCRIPTION
    1. Builds psprecomp (allegrexrecomp).
    2. Extracts the ISO's file tree to work\root\disc (what disc0:/ maps to),
       and copies the PSP's system fonts (flash0:/font/*.pgf, from PPSSPP's
       install or -Fonts) to work\root\flash0\font.
    3. Copies the decrypted EBOOT to work\ and recompiles it to C in work\gen.
    4. Configures and builds mhfu_host.

    Everything lands in work\, which is gitignored. No game data or generated
    code is ever committed.

    The region comes from the EBOOT: the Europe one (ULES01213) builds in
    work\ and build\, the USA one (ULUS10391) in work\us\ and build-us\, so
    the two can sit side by side (run.ps1 -Region US runs the second). The
    Japanese Monster Hunter Portable 2nd G (ULJM05500) builds in work\jp\ and
    build-jp\ (run.ps1 -Region JP).

.EXAMPLE
    .\scripts\setup.ps1 -Iso "..\Monster Hunter Freedom Unite (Europe) (PSP) (PSN).iso" `
                        -Eboot ..\mhfu\ULES01213_EBOOT.BIN
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Iso,
    # The *decrypted* EBOOT, e.g. PPSSPP's PSP\SYSTEM\DUMP\ULES01213_EBOOT.BIN
    [Parameter(Mandatory = $true)] [string]$Eboot,
    [string]$Config = "Release",
    # Skip re-extracting the disc tree if work\root\disc already exists.
    [switch]$KeepDisc,
    # A folder holding the PSP fonts (jpn0.pgf, ltn0.pgf ...). By default
    # they are taken from PPSSPP's install (assets\flash0\font).
    [string]$Fonts,
    # EU, US or JP; by default told from the EBOOT (its size).
    [ValidateSet("", "EU", "US", "JP")] [string]$Region = ""
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot

$Iso   = (Resolve-Path $Iso).Path
$Eboot = (Resolve-Path $Eboot).Path

# The region: PPSSPP's dumps of the two decrypted EBOOTs differ in size (the
# US .text is 0x120 longer; host/region.h).
$sizes = @{ 1910384 = "EU"; 1910672 = "US"; 1890112 = "JP" }
$ebootSize = [int](Get-Item $Eboot).Length
if (-not $Region) {
    if (-not $sizes.ContainsKey($ebootSize)) {
        throw "$Eboot ($ebootSize bytes) is not the Europe (ULES01213), USA (ULUS10391) or Japan (ULJM05500) EBOOT; pass -Region to try anyway"
    }
    $Region = $sizes[$ebootSize]
}
$gameId  = @{ EU = "ULES01213"; US = "ULUS10391"; JP = "ULJM05500" }[$Region]
$sub     = @{ EU = ""; US = "us"; JP = "jp" }[$Region]
$work    = if ($sub) { Join-Path $repo "work\$sub" } else { Join-Path $repo "work" }
$build   = if ($sub) { Join-Path $repo "build-$sub" } else { Join-Path $repo "build" }
$seedDir = if ($sub) { Join-Path $repo "seeds\$sub" } else { Join-Path $repo "seeds" }
$root   = Join-Path $work "root"
$disc   = Join-Path $root "disc"
$gen    = Join-Path $work "gen"
$ovlGen = Join-Path $work "gen_ovl"
New-Item -ItemType Directory -Force $work | Out-Null
Set-Content -Path (Join-Path $work "region.txt") -Value $Region -Encoding ASCII
# CMake takes the generated code from this work folder.
$cmakeDirs = @("-DMHFU_GEN_DIR=$gen", "-DMHFU_OVL_DIR=$ovlGen")

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

# --- 1. the toolkit -----------------------------------------------------------

# Local fixes to psprecomp that are not upstream yet. The patches stack (a
# later one can change lines an earlier one added), so they are not checked one
# by one: psprecomp's tracked files are reset to the submodule commit and the
# whole series is applied in order. Do not edit psprecomp\ by hand -- add a
# patch under patches\psprecomp\ instead, or the next setup will undo it.
#
# The series is written against upstream $upstream. Once psprecomp is on any
# other commit, the patches are taken to be committed there, and setup leaves
# psprecomp alone: re-applying would fail, and the reset would throw away
# whatever is being edited in it.
$psprecomp = Join-Path $repo "psprecomp"
$upstream = "a10edc9"
$patches = @(Get-ChildItem (Join-Path $repo "patches\psprecomp\*.patch") -ErrorAction SilentlyContinue |
    Sort-Object Name)
# git writes progress to stderr, which 'Stop' would turn into an exception in
# Windows PowerShell; exit codes are checked instead.
$ErrorActionPreference = 'Continue'
$head = (git -C $psprecomp rev-parse --short=7 HEAD).Trim()
if ($patches.Count -and $head -ne $upstream) {
    Step "psprecomp is at $head, not upstream ${upstream}: patches taken as committed, not applied"
} elseif ($patches.Count) {
    Step "Applying $($patches.Count) psprecomp patch(es)"
    git -C $psprecomp checkout -- .
    if ($LASTEXITCODE) { throw "could not reset psprecomp" }
    foreach ($p in $patches) {
        git -C $psprecomp apply --ignore-whitespace $p.FullName
        if ($LASTEXITCODE) { throw "could not apply $($p.Name)" }
        Write-Host "  $($p.Name)"
    }
}
$ErrorActionPreference = 'Stop'

Step "Building psprecomp ($Region, $gameId, in $build)"
cmake -S $repo -B $build @cmakeDirs
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $build --config $Config --target allegrexrecomp
if ($LASTEXITCODE) { throw "allegrexrecomp build failed" }

$tool = @(
    "$build\psprecomp\tools\allegrexrecomp\$Config\allegrexrecomp.exe",
    "$build\psprecomp\tools\allegrexrecomp\allegrexrecomp.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $tool) { throw "allegrexrecomp.exe not found under $build" }

# --- 2. the disc tree ---------------------------------------------------------

New-Item -ItemType Directory -Force $root, (Join-Path $root "ms") | Out-Null
# run.ps1 hands this to the host, which reads disc0: straight from the image:
# MHFU opens parts of DATA.BIN by sector number, which a folder cannot answer.
Set-Content -Path (Join-Path $work "iso.txt") -Value $Iso -Encoding UTF8

if ($KeepDisc -and (Test-Path (Join-Path $disc "PSP_GAME"))) {
    Step "Keeping existing disc tree"
} else {
    Step "Extracting the disc tree"
    if (Test-Path $disc) { Remove-Item -Recurse -Force $disc }
    New-Item -ItemType Directory -Force $disc | Out-Null

    $sevenZip = @("7z.exe", "$env:ProgramFiles\7-Zip\7z.exe") |
        Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
    if ($sevenZip) {
        & $sevenZip x $Iso "-o$disc" -y | Out-Null
        if ($LASTEXITCODE) { throw "7-Zip could not extract $Iso" }
    } else {
        # Windows can mount an ISO natively; copy its contents and unmount.
        $img = Mount-DiskImage -ImagePath $Iso -PassThru
        try {
            $letter = ($img | Get-Volume).DriveLetter
            Copy-Item -Recurse -Force "${letter}:\*" $disc
        } finally {
            Dismount-DiskImage -ImagePath $Iso | Out-Null
        }
    }
    if (-not (Test-Path (Join-Path $disc "PSP_GAME\USRDIR"))) {
        throw "Extraction finished but $disc\PSP_GAME\USRDIR is missing"
    }
}

# The PSP's system fonts. MHFU draws all its text with sceLibFont, which reads
# these; they are firmware, not part of the game. PPSSPP ships them.
$fontDst = Join-Path $root "flash0\font"
$fontCandidates = @(
    $Fonts,
    "$env:ProgramFiles\PPSSPP\assets\flash0\font",
    "${env:ProgramFiles(x86)}\PPSSPP\assets\flash0\font",
    "$env:LOCALAPPDATA\Programs\PPSSPP\assets\flash0\font",
    "$([Environment]::GetFolderPath('MyDocuments'))\PPSSPP\PSP\flash0\font"
) | Where-Object { $_ }
$fontSrc = $fontCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "ltn0.pgf") } | Select-Object -First 1
if ($fontSrc) {
    Step "Copying PSP fonts from $fontSrc"
    New-Item -ItemType Directory -Force $fontDst | Out-Null
    Copy-Item (Join-Path $fontSrc "*.pgf") $fontDst -Force
} elseif (Test-Path (Join-Path $fontDst "ltn0.pgf")) {
    Step "Keeping the PSP fonts in $fontDst"
} else {
    Write-Warning ("PSP fonts not found: game text will be blank. Looked in:`n  " +
        ($fontCandidates -join "`n  ") + "`nPass -Fonts <folder with ltn0.pgf>, e.g. PPSSPP's assets\flash0\font.")
}

# --- 3. overlays ---------------------------------------------------------------

# MHFU loads part of its code from DATA.BIN while it runs ("overlays"). The
# host saves each one it meets that has no recompiled version yet into
# work\overlays\; every file there is recompiled here, so each run of
# setup + run.ps1 gets the game further. Symbols are renamed per overlay
# (ov<hash>_func_...), because several overlays share one load address.
# Overlays go first: the EBOOT functions they call are seeds for the EBOOT's
# own recompile, since nothing inside the EBOOT calls some of them.
Step "Recompiling overlays"
$ovlSrc = Join-Path $work "overlays"
# A new work folder (the first setup of a region) gets every overlay at once,
# out of DATA.BIN (tools/re/databin.py), instead of one run at a time.
$dataBin = Join-Path $disc "PSP_GAME\USRDIR\DATA.BIN"
if (-not (Get-ChildItem $ovlSrc -Filter "slot*.bin" -ErrorAction SilentlyContinue) -and (Test-Path $dataBin)) {
    New-Item -ItemType Directory -Force $ovlSrc | Out-Null
    python (Join-Path $repo "tools\re\databin.py") $Eboot $dataBin overlays $ovlSrc | Select-Object -Last 1
    if ($LASTEXITCODE) { throw "could not take the overlays out of DATA.BIN" }
}
if (Test-Path $ovlGen) { Remove-Item -Recurse -Force $ovlGen }
New-Item -ItemType Directory -Force $ovlGen, $ovlSrc | Out-Null
$mainSeeds = Join-Path $work "main_seeds.txt"
Set-Content -Path $mainSeeds -Value "# EBOOT seeds (written by setup.ps1)" -Encoding ASCII
$ebootSeeds = Join-Path $seedDir "eboot.txt"
if (Test-Path $ebootSeeds) { Get-Content $ebootSeeds | Add-Content -Path $mainSeeds -Encoding ASCII }
else { Write-Warning "no $ebootSeeds (tools/region_seeds.py makes the US ones from the EU's)" }

Get-ChildItem (Join-Path $repo "tools") -File | ForEach-Object { $_.LastWriteTime = Get-Date }
cmake --build $build --config $Config --target mhfu_ovl
if ($LASTEXITCODE) { throw "mhfu_ovl build failed" }
$ovlTool = @("$build\$Config\mhfu_ovl.exe", "$build\mhfu_ovl.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ovlTool) { throw "mhfu_ovl.exe not found under $build" }

$seen = @{}
$table = @()
# The built-in code mods (tools/mods_convert.py) are recompiled the same way
# (an older conversion's copies out first: their hashes changed).
# Each region has its own conversion: mods\builtin (EU), mods\builtin\us (USA,
# the build they were written for: tools/mods_convert.py --region US).
Get-ChildItem $ovlSrc -Filter "mod_*.bin" | Remove-Item -Force
$modsDir = if ($sub) { Join-Path $repo "mods\builtin\$sub" } else { Join-Path $repo "mods\builtin" }
$builtinMods = @(Get-ChildItem $modsDir -Filter *.bin -ErrorAction SilentlyContinue)
foreach ($mod in $builtinMods) {
    Copy-Item -Force $mod.FullName (Join-Path $ovlSrc ("mod_" + $mod.Name))
}
foreach ($bin in Get-ChildItem $ovlSrc -Filter *.bin | Sort-Object Name) {
    $elf = Join-Path $ovlGen ($bin.BaseName + ".elf")
    $ErrorActionPreference = 'Continue'
    $line = & $ovlTool $bin.FullName $elf $mainSeeds 2>$null
    $rc = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($rc -or -not $line) { Write-Host "  skipped $($bin.Name) (not an overlay)"; continue }
    $hash, $base, $name = ("$line".Trim() -split ' ', 3)
    if ($seen.ContainsKey($hash)) { Remove-Item $elf; continue }
    $seen[$hash] = 1

    $tag = "ov$hash"
    $dir = Join-Path $ovlGen $tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    # Functions only reached through a pointer built at run time, found from
    # dispatch misses: seeds\ov<hash>.txt, one hex address per line.
    $ovlSeeds = Join-Path $seedDir "$tag.txt"
    if (Test-Path $ovlSeeds) { & $tool emit $elf $dir $tag --seeds $ovlSeeds | Out-Null }
    else { & $tool emit $elf $dir $tag | Out-Null }
    if ($LASTEXITCODE) { throw "allegrexrecomp emit failed for $($bin.Name)" }
    Remove-Item $elf
    foreach ($f in Get-ChildItem $dir -File) {
        $text = [IO.File]::ReadAllText($f.FullName)
        $text = [regex]::Replace($text, '\bpsp_(func|at|body|import)_([0-9A-F]{8})\b', "${tag}_`$1_`$2")
        $text = $text.Replace("psp_recomp_register", "${tag}_register")
        [IO.File]::WriteAllText($f.FullName, $text)
    }
    $safe = $name -replace '[^A-Za-z0-9._-]', '_'
    $table += "    { 0x${hash}u, 0x${base}u, `"$safe`", ${tag}_register },"
    Write-Host "  $safe ($hash) at 0x$base"
}

if ($table.Count) {
    $decls = ($seen.Keys | Sort-Object | ForEach-Object { "void ov${_}_register(void);" }) -join "`n"
    $src = @"
/* Written by scripts/setup.ps1 from work\overlays\ -- do not edit. */
#include "overlays.h"

$decls

const mhfu_overlay mhfu_overlays[] = {
$($table -join "`n")
};
const int mhfu_noverlays = $($table.Count);
"@
    [IO.File]::WriteAllText((Join-Path $ovlGen "overlays.c"), $src)
}
Write-Host "  $($table.Count) overlay(s) recompiled"

# --- 4. recompile the EBOOT --------------------------------------------------

Step "Recompiling the EBOOT"
$localEboot = Join-Path $work "${gameId}_EBOOT.BIN"
Copy-Item -Force $Eboot $localEboot
& $tool info $localEboot | Tee-Object -FilePath (Join-Path $work "info.txt")
# emit does not create its output directory; start it empty so no stale unit
# from an earlier run gets linked in.
if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
New-Item -ItemType Directory -Force $gen | Out-Null
& $tool emit $localEboot $gen mhfu --seeds $mainSeeds
if ($LASTEXITCODE) { throw "allegrexrecomp emit failed" }
# What host/region.h builds for (no file: Europe).
if ($Region -ne "EU") {
    [IO.File]::WriteAllText((Join-Path $gen "mhfu_region_gen.h"),
        "/* Written by scripts/setup.ps1: this build is the $Region one. */`n#define MHFU_REGION_$Region 1`n")
}
# The 60 fps mode, behind a switch (tools/fps60_patch.py). Its patches are the
# EU cheat's; --region US / JP moves them to that build's addresses.
$regionArgs = @()
if ($Region -ne "EU") { $regionArgs = @("--region", $Region) }
python (Join-Path $repo "tools\fps60_patch.py") $gen $ovlGen @regionArgs
if ($LASTEXITCODE) { throw "the 60 fps patch did not apply" }
# Built-in mods (widescreen, mods\builtin or mods\builtin\us), behind switches
# (tools/mods_patch.py); it also writes their data, gen_ovl\mods_builtin.c.
python (Join-Path $repo "tools\mods_patch.py") $gen $ovlGen @regionArgs
if ($LASTEXITCODE) { throw "the mods patch did not apply" }

# --- 5. the host --------------------------------------------------------------

Step "Building mhfu_host"
# Files unpacked from a zip can carry timestamps older than the last build
# (zip times have no time zone), and MSBuild then skips them. The host is two
# small files: always mark them changed.
Get-ChildItem (Join-Path $repo "host") -File | ForEach-Object { $_.LastWriteTime = Get-Date }
cmake -S $repo -B $build @cmakeDirs
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $build --config $Config --target mhfu_host
if ($LASTEXITCODE) { throw "mhfu_host build failed" }

$runArgs = ""
if ($Region -ne "EU") { $runArgs = " -Region $Region" }
Write-Host "`nDone ($Region). Run it with:  .\scripts\run.ps1$runArgs" -ForegroundColor Green
