# Linux and Steam Deck build

The Steam Deck is x86_64 Linux (SteamOS), so one Linux build serves both. It
uses the same recompiled C as Windows (`work\gen`, `work\<region>\...` from
`setup.ps1`): nothing is set up again.

## From Windows (WSL)

Once, in an elevated PowerShell if WSL is not installed yet: `wsl --install -d Ubuntu-24.04`.
Then the packages, once:

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config git nasm ccache \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
  libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libgles-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
  libusb-1.0-0-dev libibus-1.0-dev zlib1g-dev
```

Build (PowerShell, from the repo):

```powershell
.\scripts\build-linux.ps1                       # Europe only, from work\gen
.\scripts\build-linux.ps1 -Universal -Package   # every release set up -> release\v<version>\...-linux-x64-universal.tar.gz
```

On Linux itself: `scripts/build-linux.sh [--universal] [--package]`.

- FFmpeg is built static the first time (`scripts/build-ffmpeg-linux.sh`, only
  H.264 and ATRAC3/3+, into `deps/ffmpeg-linux`), so the game needs no FFmpeg
  on the player's system.
- The build folder is `~/.cache/mhfu/build-linux[-universal]` inside WSL (objects
  on WSL's own disk, much faster than `/mnt/e`), and **ccache** keeps every
  object (20 GB, `ccache -s` shows hits): after a setup that changed one
  overlay, only that overlay compiles again; after a clean build folder,
  everything comes from the cache.
- HTTP (update check, RetroAchievements) goes through the system's
  `libcurl.so.4`, opened at run time (`host/curl_dl.c`); without it the game
  still runs.
- The binaries need glibc 2.39+ (built on Ubuntu 24.04): SteamOS 3.6+,
  Ubuntu 24.04+, Fedora 40+, Arch. For older systems, build in an older
  distro (e.g. a Steam Runtime "sniper" container).

## Testing without a Deck

- WSLg shows Linux windows on the Windows desktop: run
  `~/.cache/mhfu/build-linux/Release/mhfu_launcher` in WSL, or the host
  directly as `run.ps1` would (EBOOT, root, `--iso`, `--window 2`).
  A controller plugged into Windows is not seen inside WSL (no USB passthrough
  without usbipd); the keyboard works.
- Steam Deck specifics:
  - Steam Input presents the Deck's controls as an Xbox-style pad; SDL3 maps
    it like any other (gamepad.c).
  - **R3** (right stick pressed) opens the in-game menu (no F1 on a Deck);
    the touch screen works in it, and on desktop Linux the mouse's clicks
    count as touches (`SDL_HINT_MOUSE_TOUCH_EVENTS`).
  - The launcher can be driven with the controller (ImGui gamepad
    navigation) or the touch screen. Game Mode runs it under gamescope;
    "Fullscreen" on the Play page is best there.
  - The Deck's screen is 1280x800 (16:10): Widescreen fits it.
