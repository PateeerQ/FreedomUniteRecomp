#!/usr/bin/env bash
# FFmpeg for the Linux build (desktop Linux and the Steam Deck): only what the
# game uses -- the H.264 decoder and parser (movies; the HEVC parser too, see
# build-ffmpeg-android.sh), ATRAC3 and ATRAC3+ (music, movie sound), swscale and
# swresample -- as static x86_64 libraries, into deps/ffmpeg-linux (include/,
# lib/), where CMakeLists.txt looks on Linux. Static, so the game does not
# depend on whichever FFmpeg a distro (or SteamOS) ships. LGPL, no programs.
#
# Run on Linux or in WSL (scripts/build-linux.sh calls it when needed):
#   scripts/build-ffmpeg-linux.sh [path/to/ffmpeg-source]
# The source defaults to deps/ffmpeg-src (FFmpeg 7.1.1, as on Windows/Android).
# The build folder lives outside the repository's drive when that is a
# Windows one (/mnt/...): configure and make are many times faster there.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
src="${1:-$repo/deps/ffmpeg-src}"
out="$repo/deps/ffmpeg-linux"
build="${MHFU_FFMPEG_BUILD:-$HOME/.cache/mhfu/ffmpeg-linux-build}"

[ -f "$src/configure" ] || { echo "no FFmpeg source in $src"; exit 1; }

mkdir -p "$build" && rm -rf "${build:?}"/* && cd "$build"

"$src/configure" \
    --prefix="$out" \
    --arch=x86_64 \
    --extra-cflags="-O2 -fPIC" \
    --enable-static --disable-shared --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-network \
    --disable-autodetect --disable-iconv \
    --disable-everything \
    --enable-decoder=h264,atrac3,atrac3al,atrac3p,atrac3pal \
    --enable-parser=h264,hevc \
    --enable-swscale --enable-swresample

make -j"$(nproc)"
make install
echo "FFmpeg for Linux x86_64 in $out"
