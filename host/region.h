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
#define MHFU_IS_US         1                /* for C expressions (MHFU_REGION_US is undefined in EU) */
#define MHFU_GAME_ID       "ULUS10391"      /* save folder, EBOOT dump name */
#define MHFU_GAME_ID_DASH  "ULUS-10391"     /* CWCheat */
#define MHFU_REGION_NAME   "USA"
#define MHFU_REGION_TAG    "US"
#define MHFU_WORK_SUB      "us"             /* work/us: setup.ps1's folder for this region */
#define MHFU_EBOOT_CODE_END 0x0010E7D8u     /* CWCheat offset where the EBOOT's code ends (EU + 0x120) */
#define MHFU_EBOOT_SIZE    1910672u         /* PPSSPP's dump of the decrypted EBOOT */
#define MHFU_ADDR_OVL_TABLE     0x089CBB90u /* overlay slot starts (EU + 0x120) */
#define MHFU_ADDR_SAVE_MANAGER  0x09A44DA0u /* unverified: EU + 0x140 (heap) */
#define MHFU_ADDR_WEAPON_CLASS  0x090B3768u /* EU + 0x140 (heap): weapon buttons right on the phone, 2026-11-05 */
#define MHFU_ADDR_WEAPON_OUT    0x090B3B92u /* EU + 0x140 (heap): the same */
#define MHFU_ADDR_FPS60_TABLE   0x09B07D98u /* game_task's data: 2.0, 3.0, 4.0 found there in the US DATA.BIN */
#define MHFU_ADDR_VFPU_ATAN     0u          /* the MHFU_TEST_ATAN self-test: EU only */
/* 60 fps: the EU cheat moved to US addresses (tools/fps60_patch.py --region US);
 * the built-in mods: made for this build, which they were written for
 * (tools/mods_convert.py --region US, mods/builtin/us). */
#define MHFU_HAS_FPS60          1
#define MHFU_HAS_BUILTIN_MODS   1
#else
#define MHFU_REGION_EU     1
#define MHFU_IS_US         0
#define MHFU_GAME_ID       "ULES01213"
#define MHFU_GAME_ID_DASH  "ULES-01213"
#define MHFU_REGION_NAME   "Europe"
#define MHFU_REGION_TAG    "EU"
#define MHFU_WORK_SUB      ""
#define MHFU_EBOOT_CODE_END 0x0010E6B8u
#define MHFU_EBOOT_SIZE    1910384u
#define MHFU_ADDR_OVL_TABLE     0x089CBA70u
#define MHFU_ADDR_SAVE_MANAGER  0x09A44C60u
#define MHFU_ADDR_WEAPON_CLASS  0x090B3628u
#define MHFU_ADDR_WEAPON_OUT    0x090B3A52u
#define MHFU_ADDR_FPS60_TABLE   0x09B07C98u
#define MHFU_ADDR_VFPU_ATAN     0x088147C8u
#define MHFU_HAS_FPS60          1
#define MHFU_HAS_BUILTIN_MODS   1
#endif

#define MHFU_EBOOT_FILE MHFU_GAME_ID "_EBOOT.BIN"

#endif
