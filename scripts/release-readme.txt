Monster Hunter: Freedom Unite Recomp -- Windows
Maintained by PateeerQ -- https://github.com/PateeerQ/FreedomUniteRecomp

A native port of Monster Hunter Freedom Unite / Portable 2nd G (PSP): the
game's own code, statically recompiled, on a reimplementation of the PSP's
firmware. No game files come with it: you need your own copy.

RELEASES IT PLAYS
  Monster Hunter Freedom Unite (Europe)      ULES01213
  Monster Hunter Freedom Unite (USA)         ULUS10391
  Monster Hunter Portable 2nd G (Japan)      ULJM05500
  FUComplete (the English MHP2G fan translation, its .iso as it is)
  The launcher tells which one from the .iso you choose. Every release plays
  with every other in multiplayer rooms.

WHAT YOU NEED
  1. Your dump of the game, as an .iso
  2. Its decrypted EBOOT: in PPSSPP, Settings > Tools > Developer tools >
     "Dump decrypted EBOOT.BIN" while the game runs (<game id>_EBOOT.BIN).
     Not for FUComplete: its EBOOT is taken from the .iso.
  3. The PSP's fonts: the .pgf files from PPSSPP's assets\flash0\font,
     copied into work\root\flash0\font here

START
  Run mhfu_launcher.exe. Under Advanced choose the ISO (and the EBOOT); the
  Play page checks everything, then DEPART.

  Keyboard: W/A/S/D move, arrows = D-pad (camera), K/Space = Cross, L = Circle,
  J = Square, I = Triangle, Q/E = L/R, Enter = Start, V = Select (change them on
  the Keyboard page). DualSense, DualShock and Xbox controllers work too.

  Multiplayer: in the game's window, Multiplayer > Create Server on one PC
  (Internet: players anywhere, or This network: same Wi-Fi), Join Server on
  the others and pick the room (or type its code, MH-XXXX), then go online in
  the game (Gathering Hall). PC and Android play together. No port forwarding.

  Chat: while in a room, press SELECT (keyboard V/Backspace, controller
  Create/Share), type, Enter. Lines show top-left; joins and leaves too.

  Texture packs (optional): a pack made for PPSSPP works as it is. Launcher >
  Mods > Texture pack: choose its folder (the one with textures.ini, or the one
  above its MAIN), and a Style if the pack has ADDONS.

  Saves are in work\root\ms\PSP\SAVEDATA for Europe, work\us\..., work\jp\...
  and work\fuc\... for the others (or work\profiles\<name>).

FILES
  mhfu_launcher.exe   the launcher
  mhfu_host_*.exe     the game, one per release
  *.dll               FFmpeg 7.1 (LGPL 2.1), for movies and music

LICENSE
  This program is free software under the GNU General Public License,
  version 3 (the source: https://github.com/PateeerQ/FreedomUniteRecomp).
  It includes libkirk (GPL 3) to read saves from a PSP or PPSSPP.

NOTICE
  Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of
  Capcom. FFmpeg is licensed under the LGPL 2.1 (https://ffmpeg.org/legal.html);
  these DLLs are the unmodified BtbN LGPL shared build, n7.1.1. Texture
  decoding by Basis Universal (Apache 2.0).
