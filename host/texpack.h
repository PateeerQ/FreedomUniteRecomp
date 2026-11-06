/* MHFU-Android -- texture packs (PPSSPP's format): high-resolution pictures in
 * place of the game's own textures.
 *
 * A pack is a folder with a textures.ini ([hashes]: "<16 hex cache key><8 hex
 * hash> = path/to/file.png|.ktx2") and the files it names. The hash is
 * PPSSPP's "quick" hash of the texture's bytes in guest memory, the same
 * function, so packs made for PPSSPP match here. KTX2 files (Basis Universal
 * UASTC/ETC1S, zstd) are transcoded with basisu (host/ktx2.cpp), PNG files read
 * with stb_image. Pictures load on a thread of their own; the game's texture
 * shows until one is ready (psprecomp's psp_render_gl_set_replacer).
 *
 * Where: MHFU_TEXPACK=<folder>, else <root>/textures/<game id>. A folder with
 * MAIN/textures.ini (as packs are shipped) is read from MAIN. The release's own
 * ini is preferred when the pack has one: textures_FU_EU.ini in the Europe
 * build, textures_FU_US.ini in the USA one, else textures.ini. */
#ifndef MHFU_TEXPACK_H
#define MHFU_TEXPACK_H

/* Finds and reads the pack (see above); 0 if one is in use. */
int mhfu_texpack_init(const char *root);
/* "N replacements, M loaded, K missing files" for the run's report. */
void mhfu_texpack_report(void);
/* How many textures the pack in use lists (0: none in use). */
int mhfu_texpack_textures(void);
/* The pack's styles: folders under ADDONS/ (e.g. "NEW/Normal/PSX") whose files
 * take the place of MAIN's. MHFU_TEXPACK_STYLE=<one of them> picks one at
 * start; none: MAIN as it is. */
int mhfu_texpack_styles(void);
const char *mhfu_texpack_style(int i);
const char *mhfu_texpack_style_in_use(void);

#endif
