# Profile-guided optimisation for the Android app (clang PGO), in three steps:
#
#   .\scripts\pgo-android.ps1 -Gen          # builds and installs a training APK (counts every branch)
#     ... play on the phone: the title, the village, a quest or two, an area change ...
#     (the counts are written to files/pgo/ every ~2000 game frames; close the game when done)
#   .\scripts\pgo-android.ps1 -Merge        # pulls files/pgo/*.profraw and merges work\pgo\mhfu.profdata
#   .\scripts\release.ps1 ...               # builds with the profile when work\pgo\mhfu.profdata exists
#                                           # (or by hand: gradlew assembleRelease -Pmhfu.pgo=use)
#
# A profile fits the release (EU/US/JP/FUC) it was played with; play several and
# merge them all, each host takes its own. -Region picks the APK (default ALL,
# the universal one: the training build of every release takes long; -Region FUC
# builds only FUComplete, as its own app com.pateeerq.mhfu.fuc, with its own saves).
# The training APK is slow (about half speed): it is only for collecting counts.
param(
    [switch]$Gen,
    [switch]$Merge,
    [string]$Region = "ALL",
    [switch]$Keep               # -Merge: keep the old .profraw files in work\pgo\raw
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$pgo = "$repo\work\pgo"
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
if (-not (Test-Path $adb)) { $adb = "adb" }
$ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin"
$sub = $Region.ToLower()
$pkg = if ($Region -eq "ALL" -or $Region -eq "EU") { "com.pateeerq.mhfu" } else { "com.pateeerq.mhfu.$sub" }

if (-not $Gen -and -not $Merge) { throw "say -Gen (build and install the training APK) or -Merge (pull and merge the counts)" }

if ($Gen) {
    $jdk = Get-ChildItem "$env:USERPROFILE\.jdks\jbr-21*", "$env:USERPROFILE\.jdks\*-21*" -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path "$($_.FullName)\bin\java.exe" } | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
    $ErrorActionPreference = "Continue"
    & "$repo\android\gradlew.bat" -p "$repo\android" assembleRelease "-Pmhfu.region=$Region" "-Pmhfu.pgo=gen" --console=plain 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code) { throw "the training build failed" }
    $apk = Get-ChildItem "$repo\android\app\build\outputs\apk\release\*.apk" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $apk -or $apk.LastWriteTime -lt (Get-Date).AddMinutes(-5)) { throw "no fresh APK: the build did not make one" }
    Write-Host "Installing $($apk.Name) (confirm on the phone)..." -ForegroundColor Cyan
    & $adb install -r $apk.FullName
    if ($LASTEXITCODE) { throw "adb install failed" }
    & $adb shell am force-stop $pkg
    Write-Host "Now play on the phone (a few minutes in the village and a quest), then run -Merge." -ForegroundColor Green
}

if ($Merge) {
    New-Item -ItemType Directory -Force "$pgo\raw" | Out-Null
    if (-not $Keep) { Remove-Item "$pgo\raw\*.profraw" -ErrorAction SilentlyContinue }
    & $adb pull "/sdcard/Android/data/$pkg/files/pgo/." "$pgo\raw"
    if ($LASTEXITCODE) { throw "adb pull failed (is the training APK installed and played?)" }
    $raw = Get-ChildItem "$pgo\raw" -Recurse -Filter *.profraw
    if (-not $raw) { throw "no .profraw on the phone yet" }
    & "$ndk\llvm-profdata.exe" merge -o "$pgo\mhfu.profdata" $raw.FullName
    if ($LASTEXITCODE) { throw "llvm-profdata merge failed" }
    Write-Host "work\pgo\mhfu.profdata from $($raw.Count) runs; release.ps1 (or -Pmhfu.pgo=use) builds with it." -ForegroundColor Green
    Write-Host "The phone keeps its .profraw: adb shell rm -r /sdcard/Android/data/$pkg/files/pgo to start over."
}
