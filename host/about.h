/* MHFU-Android -- who made this, for the About pages: the launcher's, the
 * desktop window's Help menu and (from the same words) the Android app's
 * MhfuActivity.openAbout. The Android version is versionName in
 * android/app/build.gradle; keep the two the same. */
#ifndef MHFU_ABOUT_H
#define MHFU_ABOUT_H

#define MHFU_APP_NAME   "Monster Hunter: Freedom Unite Recomp"
#define MHFU_VERSION    "0.4.3"
#define MHFU_MAINTAINER "PateeerQ"
#define MHFU_REPO       "PateeerQ/FreedomUniteRecomp"
#define MHFU_REPO_URL   "https://github.com/" MHFU_REPO

/* The internet relay rooms go through unless <root>/net.cfg names another
 * (relay=host:port): PateeerQ's mhfu_hub. Its address is not in the sources:
 * CMake reads it from server/relay.txt (server/ is gitignored) and defines
 * MHFU_DEFAULT_RELAY; a build without that file has rooms on the LAN only. */
#ifndef MHFU_DEFAULT_RELAY
#define MHFU_DEFAULT_RELAY ""
#endif

#define MHFU_ABOUT_TEXT                                                                                   \
    "A native port of Monster Hunter Freedom Unite (PSP, ULES01213), statically recompiled.\n\n"          \
    "Maintained by " MHFU_MAINTAINER ".\n" MHFU_REPO_URL "\n\n"                                           \
    "Built on psprecomp (sp00nznet), SDL3, FFmpeg (LGPL), rcheevos, Dear ImGui and stb.\n"                 \
    "Touch button icons by Lorc, Delapouite and SeregaCthtuf, game-icons.net (CC BY 3.0).\n\n"                         \
    "Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of Capcom. "                \
    "Play with your own copy of the game: no game files come with this program."

#endif
