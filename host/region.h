/* MHFU-Android -- which release of the game this build was recompiled from.
 *
 * A build carries one region's recompiled code: setup.ps1 sees which EBOOT it
 * was given and writes <gen>/mhfu_region_gen.h ("#define MHFU_REGION_US 1"
 * for the US one). Without that file the build is the EU one, as before.
 *
 * The US release (ULUS10391) is the EU one (ULES01213) with .text 0x120
 * longer near its end: .data/.bss sit 0x120 higher, heap objects 0x140,
 * overlay slots 0x100 (CLAUDE.md; tools/mods_convert.py's us2eu checked the
 * same rules against a quest's memory). The US addresses below follow those
 * rules; the ones marked "unverified" have not been looked at in a US run. */
#ifndef MHFU_REGION_H
#define MHFU_REGION_H

#if defined(__has_include)
#if __has_include("mhfu_region_gen.h")
#include "mhfu_region_gen.h"
#endif
#endif

#if MHFU_REGION_US
#define MHFU_GAME_ID       "ULUS10391"      /* save folder, EBOOT dump name */
#define MHFU_GAME_ID_DASH  "ULUS-10391"     /* CWCheat */
#define MHFU_REGION_NAME   "USA"
#define MHFU_EBOOT_SIZE    1910672u         /* PPSSPP's dump of the decrypted EBOOT */
#define MHFU_ADDR_OVL_TABLE     0x089CBB90u /* overlay slot starts (EU + 0x120) */
#define MHFU_ADDR_SAVE_MANAGER  0x09A44DA0u /* unverified: EU + 0x140 (heap) */
#define MHFU_ADDR_WEAPON_CLASS  0x090B3768u /* unverified: EU + 0x140 (heap) */
#define MHFU_ADDR_WEAPON_OUT    0x090B3B92u /* unverified: EU + 0x140 (heap) */
#define MHFU_ADDR_FPS60_TABLE   0x09B07D98u /* unverified: game_task's data, EU + 0x100 */
#define MHFU_ADDR_VFPU_ATAN     0u          /* the MHFU_TEST_ATAN self-test: EU only */
#else
#define MHFU_REGION_EU     1
#define MHFU_GAME_ID       "ULES01213"
#define MHFU_GAME_ID_DASH  "ULES-01213"
#define MHFU_REGION_NAME   "Europe"
#define MHFU_EBOOT_SIZE    1910384u
#define MHFU_ADDR_OVL_TABLE     0x089CBA70u
#define MHFU_ADDR_SAVE_MANAGER  0x09A44C60u
#define MHFU_ADDR_WEAPON_CLASS  0x090B3628u
#define MHFU_ADDR_WEAPON_OUT    0x090B3A52u
#define MHFU_ADDR_FPS60_TABLE   0x09B07C98u
#define MHFU_ADDR_VFPU_ATAN     0x088147C8u
#endif

#define MHFU_EBOOT_FILE MHFU_GAME_ID "_EBOOT.BIN"

#endif
