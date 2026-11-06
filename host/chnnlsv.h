/* MHFU-Android -- the PSP's save data cipher (host/chnnlsv.c, GNU GPL 3 or later). */
#ifndef MHFU_CHNNLSV_H
#define MHFU_CHNNLSV_H

#include <stdint.h>

/* A save file a PSP or PPSSPP encrypted, decrypted in place: `data` holds *len
 * bytes and room up to the next 16 (zeroed); on success *len is the plain
 * length. mode: 1, 3 or 5 (PARAM.SFO's SAVEDATA_PARAMS 0x01 / 0x21 / 0x41); key:
 * the game's 16-byte save key (modes 3 and 5), else NULL; expected: the file's
 * hash from SAVEDATA_FILE_LIST to check against, or NULL. 0 on success, -8 if
 * the hash did not match (a wrong key or mode). */
int mhfu_save_decrypt(int mode, uint8_t *data, int *len, const uint8_t key[16], const uint8_t expected[16]);

#endif
