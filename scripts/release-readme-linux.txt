Monster Hunter: Freedom Unite Recomp -- Linux and Steam Deck
Maintained by PateeerQ -- https://github.com/PateeerQ/FreedomUniteRecomp

A native port of Monster Hunter Freedom Unite / Portable 2nd G (PSP): the
game's own code, statically recompiled, on a reimplementation of the PSP's
firmware. No game files come with it: you need your own copy.

RELEASES IT PLAYS
  Monster Hunter Freedom Unite (Europe)      ULES01213
  Monster Hunter Freedom Unite (USA)         ULUS10391
  Monster Hunter Portable 2nd G (Japan)      ULJM05500
  FUComplete (the English MHP2G fan translation, its .iso as it is)
  (a universal package has all four; the launcher tells which from the .iso)
  Every release plays with every other in multiplayer rooms, PC, Linux and
  Android alike.

WHAT YOU NEED
  1. Your dump of the game, as an .iso
  2. Its decrypted EBOOT: in PPSSPP, Settings > Tools > Developer tools >
     "Dump decrypted EBOOT.BIN" while the game runs (<game id>_EBOOT.BIN).
     Not for FUComplete: its EBOOT is taken from the .iso.
  3. The PSP's fonts: the .pgf files from PPSSPP's assets/flash0/font,
     copied into work/root/flash0/font here
  4. A 64-bit (x86_64) Linux with OpenGL 3.3 and glibc 2.38 or newer
     (SteamOS 3.6+, Ubuntu 24.04+, Fedora 40+, Arch, ...). For online
     features (update check, RetroAchievements) libcurl, which nearly every
     system has.

START (desktop Linux)
  tar xzf FreedomUniteRecomp-*.tar.gz, then run ./start.sh in the folder.
  Under Advanced choose the ISO (and the EBOOT); the Play page checks
  everything, then DEPART. ./install-desktop-entry.sh adds it to the
  application menu.

STEAM DECK
  1. In Desktop Mode, unpack the folder somewhere (for example
     /home/deck/Games/FreedomUniteRecomp, or on the SD card).
  2. Copy the .iso, the decrypted EBOOT and the fonts (see above) onto the Deck.
  3. Run ./install-desktop-entry.sh once (or skip it), then in Steam: Games >
     Add a Non-Steam Game > "Monster Hunter Freedom Unite Recomp" (or Browse
     to start.sh).
  4. Start it once in Desktop Mode to choose the ISO/EBOOT under Advanced and
     tick Fullscreen on the Play page; after that it works from Game Mode.
     The launcher can be used with the touch screen or the controller (d-pad
     and A), and the Deck's controls are the game's: A/B/X/Y = Cross/Circle/
     Square/Triangle, L1/L2 = L, R1/R2 = R, the right stick = the camera.
  Use Steam Input's "Gamepad" layout for it (the default for non-Steam games).

  Keyboard: W/A/S/D move, arrows = D-pad (camera), K/Space = Cross, L = Circle,
  J = Square, I = Triangle, Q/E = L/R, Enter = Start, V = Select (change them on
  the Keyboard page). F1 opens the room chat.

  The in-game menu (settings, multiplayer, achievements): R3 (press the right
  stick) on a controller or the Deck, F1 on a keyboard; touch it on the Deck's
  screen or click it with the mouse.

  Multiplayer: in the in-game menu's Online tab, Create Server or Join Server
  with the room's code (MH-XXXX). No port forwarding. Linux, Windows and
  Android play together.

  Saves are in work/root/ms/PSP/SAVEDATA for Europe, work/us/..., work/jp/...
  and work/fuc/... for the others (or work/profiles/<name>).

FILES
  start.sh                  starts the launcher (add this one to Steam)
  mhfu_launcher             the launcher
  mhfu_host*                the game, one per release
  install-desktop-entry.sh  adds the launcher to the application menu

LICENSE
  This program is free software under the GNU General Public License,
  version 3 (the source: https://github.com/PateeerQ/FreedomUniteRecomp).
  It includes libkirk (GPL 3) to read saves from a PSP or PPSSPP.

NOTICE
  Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of
  Capcom. FFmpeg 7.1.1 (LGPL 2.1, https://ffmpeg.org/legal.html) is linked in
  statically, unmodified, with only its H.264 and ATRAC3/ATRAC3+ decoders
  (scripts/build-ffmpeg-linux.sh in the source rebuilds it). Texture decoding
  by Basis Universal (Apache 2.0).
