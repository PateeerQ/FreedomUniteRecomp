#!/usr/bin/env bash
# The Linux build (desktop Linux and the Steam Deck: both x86_64), run on Linux
# or in WSL (Windows: scripts\build-linux.ps1 runs this in WSL for you).
#
#   scripts/build-linux.sh               one host from work/gen (Europe) + the launcher
#   scripts/build-linux.sh --universal   every release set up in work/ (mhfu_host_eu, _us, _jp, _fuc)
#   scripts/build-linux.sh --package     also writes release/v<version>/...-linux-x64[-universal].tar.gz
#
# The recompiled code is the one setup.ps1 made on Windows (work/gen, work/gen_ovl,
# work/<region>/...): C is C, nothing has to be set up again for Linux.
# FFmpeg is built static the first time (scripts/build-ffmpeg-linux.sh).
# The build folder lives on Linux's own disk (default ~/.cache/mhfu/build-linux[-universal]):
# building from /mnt/e is fine, writing objects there is slow. ccache is used when
# installed, so a rebuild after setup only compiles what changed.
#
# Needs: cmake, ninja, gcc/g++, nasm, and SDL3's dev headers (X11, Wayland, ALSA,
# PulseAudio/PipeWire, udev, dbus, ...); on Ubuntu/Debian see docs/LINUX.md.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
universal=0
package=0
build=""
for a in "$@"; do
    case "$a" in
        --universal) universal=1 ;;
        --package) package=1 ;;
        --build-dir=*) build="${a#*=}" ;;
        *) echo "unknown option $a"; exit 2 ;;
    esac
done
[ -n "$build" ] || build="$HOME/.cache/mhfu/build-linux$([ $universal = 1 ] && echo -universal)"

[ -f "$repo/deps/ffmpeg-linux/lib/libavcodec.a" ] || bash "$repo/scripts/build-ffmpeg-linux.sh"

launcher=()
command -v ccache >/dev/null && launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)

targets=(mhfu_launcher)
if [ $universal = 1 ]; then
    for r in eu us jp fuc; do
        w="$repo/work"; [ $r = eu ] || w="$repo/work/$r"
        if compgen -G "$w/gen/*.c" >/dev/null; then targets+=("mhfu_host_$r"); else echo "  $r: not set up ($w/gen is empty), left out"; fi
    done
else
    targets+=(mhfu_host)
fi
echo "Building: ${targets[*]} in $build"

cmake -S "$repo" -B "$build" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release -DMHFU_TRACE=LITE \
    -DMHFU_UNIVERSAL=$([ $universal = 1 ] && echo ON || echo OFF) "${launcher[@]}"
cmake --build "$build" --target "${targets[@]}" -j"$(nproc)"

[ $package = 1 ] || { echo "Done: $build/mhfu_launcher"; exit 0; }

# ---- the package: one folder to unpack anywhere (home, an SD card) ----
version="$(sed -n 's/^#define MHFU_VERSION "\(.*\)".*/\1/p' "$repo/host/about.h")"
name="FreedomUniteRecomp-v$version"
suffix=""; [ $universal = 1 ] && suffix="-universal"
out="$repo/release/v$version"
stage="$(mktemp -d)/$name"
mkdir -p "$stage/work/root/flash0/font" "$out"

cp "$build/Release/mhfu_launcher" "$stage/"          # launcher/CMakeLists.txt: <build>/<config>/
cp "$repo/assets/icon/mhfu.png" "$stage/"
for t in "${targets[@]}"; do
    [ "$t" = mhfu_launcher ] && continue
    cp "$build/$t" "$stage/"
done
strip "$stage"/mhfu_host* "$stage/mhfu_launcher"

cat > "$stage/start.sh" <<'EOF'
#!/bin/sh
# Starts the launcher from wherever this folder is (Steam: add this file as a non-Steam game).
cd "$(dirname "$(readlink -f "$0")")" && exec ./mhfu_launcher "$@"
EOF
chmod +x "$stage/start.sh" "$stage/mhfu_launcher" "$stage"/mhfu_host*

cat > "$stage/install-desktop-entry.sh" <<'EOF'
#!/bin/sh
# Adds the launcher to the desktop's application menu (from there, Steam's
# "Add a Non-Steam Game" lists it too).
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/freedom-unite-recomp.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Monster Hunter Freedom Unite Recomp
Exec="$here/start.sh"
Path=$here
Icon=$here/mhfu.png
Categories=Game;
Terminal=false
DESK
echo "Added: $HOME/.local/share/applications/freedom-unite-recomp.desktop"
EOF
chmod +x "$stage/install-desktop-entry.sh"

sed 's/\r$//' "$repo/scripts/release-readme-linux.txt" > "$stage/README.txt"
printf '%s\n' "Copy the PSP's .pgf fonts here (PPSSPP has them in assets/flash0/font). Without them all text is blank." \
    > "$stage/work/root/flash0/font/PUT_FONTS_HERE.txt"

tarball="$out/$name-linux-x64$suffix.tar.gz"
tar -C "$(dirname "$stage")" -czf "$tarball" "$name"
rm -rf "$(dirname "$stage")"
echo "Packaged: $tarball ($(du -h "$tarball" | cut -f1))"
