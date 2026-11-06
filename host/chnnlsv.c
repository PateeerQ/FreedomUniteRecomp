/* MHFU-Android -- the PSP's save data cipher (sceChnnlsv), for reading saves a
 * PSP or PPSSPP encrypted. Ported to C from PPSSPP's Core/HLE/sceChnnlsv.cpp and
 * SavedataParam::DecryptData (Copyright (c) 2012- PPSSPP Project, GNU GPL 2.0
 * or later), over the KIRK engine in third_party/libkirk (GNU GPL 3 or later).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. */
#include "chnnlsv.h"

#include <stdint.h>
#include <string.h>

#include "kirk_engine.h"

typedef struct { int mode; uint8_t result[16], key[16]; int key_len; } mac_ctx;
typedef struct { int mode, unkn; uint8_t crypted[16]; } cipher_ctx;

static KirkState g_kirk;
static int g_kirk_ready;
static uint8_t g_buf[2048 + 20];
static uint8_t *const g_buf2 = g_buf + 20;

static const uint8_t hash198C[16] = { 0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF };
static const uint8_t hash19BC[16] = { 0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86 };
static const uint8_t key19CC[16]  = { 0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B };
static const uint8_t key19DC[16]  = { 0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00 };
static const uint8_t key199C[16]  = { 0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72 };
static const uint8_t key19AC[16]  = { 0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03 };

static void memxor(uint8_t *d, const uint8_t *s, size_t n) { while (n--) *d++ ^= *s++; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int num_from_mode(int mode) {
    switch (mode) {
    case 1: return 3;
    case 2: return 5;
    case 3: return 12;
    case 4: return 13;
    case 6: return 17;
    default: return 16;
    }
}
static int num_from_mode2(int mode) { return mode == 1 ? 4 : mode == 3 ? 14 : 18; }
static int type_from_mode(int mode) { return (mode == 1 || mode == 2) ? 83 : (mode == 3 || mode == 4) ? 87 : 100; }

static int send_cmd(uint8_t *data, int length, int num, int encrypt) {
    put32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    put32(data + 4, 0);
    put32(data + 8, 0);
    put32(data + 12, (uint32_t)num);
    put32(data + 16, (uint32_t)length);
    return kirk_sceUtilsBufferCopyWithRange(&g_kirk, data, length + 20, data, length + 20,
                                            encrypt ? KIRK_CMD_ENCRYPT_IV_0 : KIRK_CMD_DECRYPT_IV_0) ? -257 : 0;
}

/* Modes 2, 4 and 6 need the console's fuse ID: a PSP-only save, which libkirk cannot do. */
static int send_fuse_cmd(uint8_t *data, int length, int encrypt) {
    put32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    put32(data + 4, 0);
    put32(data + 8, 0);
    put32(data + 12, 256);
    put32(data + 16, (uint32_t)length);
    return kirk_sceUtilsBufferCopyWithRange(&g_kirk, data, length + 20, data, length + 20,
                                            encrypt ? KIRK_CMD_ENCRYPT_IV_FUSE : KIRK_CMD_DECRYPT_IV_FUSE) ? -258 : 0;
}

static int sub_15B0(uint8_t *data, int aligned, uint8_t *buf, int val) {
    uint8_t sp0[16];
    memcpy(sp0, data + aligned + 4, 16);
    int res = send_cmd(data, aligned, val, 0);
    if (res) return res;
    memxor(data, buf, 16);
    memcpy(buf, sp0, 16);
    return 0;
}

static int sub_0000(uint8_t *out, uint8_t *data, int aligned, const uint8_t *data2, int *data3, int mode) {
    memcpy(out + 20, data2, 16);
    const int type = type_from_mode(mode);
    if (type == 87) memxor(out + 20, key19AC, 16);
    else if (type == 100) memxor(out + 20, key19DC, 16);
    int res = (mode == 2 || mode == 4 || mode == 6) ? send_fuse_cmd(out, 16, 0)
                                                    : send_cmd(out, 16, num_from_mode2(mode), 0);
    if (type == 87) memxor(out, key199C, 16);
    else if (type == 100) memxor(out, key19CC, 16);
    if (res) return res;

    uint8_t sp0[16], sp16[16];
    memcpy(sp16, out, 16);
    if (*data3 == 1) memset(sp0, 0, 16);
    else { memcpy(sp0, sp16, 12); put32(sp0 + 12, (uint32_t)(*data3 - 1)); }
    for (int i = 20; i < aligned + 20; i += 16) {
        memcpy(out + i, sp16, 12);
        put32(out + 12 + i, (uint32_t)*data3);
        (*data3)++;
    }
    res = sub_15B0(out, aligned, sp0, type);
    if (res) return res;
    if (aligned > 0) memxor(data, out, (size_t)aligned);
    return 0;
}

static int sub_1510(uint8_t *data, int size, uint8_t *result, int num) {
    memxor(data + 20, result, 16);
    int res = send_cmd(data, size, num, 1);
    if (res) return res;
    memcpy(result, data + size + 4, 16);
    return 0;
}

static int mac_init(mac_ctx *c, int mode) {
    c->mode = mode;
    memset(c->result, 0, 16);
    memset(c->key, 0, 16);
    c->key_len = 0;
    return 0;
}

static int mac_update(mac_ctx *c, const uint8_t *data, int length) {
    if (c->key_len < 0 || c->key_len >= 17) return -1026;
    if (c->key_len + length < 17) {
        memcpy(c->key + c->key_len, data, (size_t)length);
        c->key_len += length;
        return 0;
    }
    const int num = num_from_mode(c->mode);
    memset(g_buf2, 0, 2048);
    memcpy(g_buf2, c->key, (size_t)c->key_len);
    int len = (c->key_len + length) & 0xF;
    if (len == 0) len = 16;
    int size = c->key_len;
    c->key_len = len;
    const int diff = length - len;
    memcpy(c->key, data + diff, (size_t)len);
    for (int i = 0; i < diff; i++) {
        if (size == 2048) {
            int res = sub_1510(g_buf, 2048, c->result, num);
            if (res) return res;
            size = 0;
        }
        g_buf2[size++] = data[i];
    }
    if (size) sub_1510(g_buf, size, c->result, num);
    return 0;
}

static void shift_left(uint8_t d[16]) {
    const int carry = (d[0] & 0x80) ? 135 : 0;
    for (int i = 0; i < 15; i++) d[i] = (uint8_t)((d[i] << 1) | (d[i + 1] >> 7));
    d[15] = (uint8_t)((d[15] << 1) ^ carry);
}

static int mac_final(mac_ctx *c, uint8_t *hash, const uint8_t *key) {
    if (c->key_len < 0 || c->key_len >= 17) return -1026;
    const int num = num_from_mode(c->mode);
    memset(g_buf2, 0, 16);
    int res = send_cmd(g_buf, 16, num, 1);
    if (res) return res;
    uint8_t d1[16], d2[16];
    memcpy(d1, g_buf2, 16);
    shift_left(d1);
    if (c->key_len < 16) {
        shift_left(d1);
        c->key[c->key_len] = 0x80;
        if (c->key_len + 1 < 16) memset(c->key + c->key_len + 1, 0, (size_t)(16 - c->key_len - 1));
    }
    memxor(c->key, d1, 16);
    memcpy(g_buf2, c->key, 16);
    memcpy(d2, c->result, 16);
    res = sub_1510(g_buf, 16, d2, num);
    if (res) return res;
    if (c->mode == 3 || c->mode == 4) memxor(d2, hash198C, 16);
    else if (c->mode == 5 || c->mode == 6) memxor(d2, hash19BC, 16);
    if (c->mode == 2 || c->mode == 4 || c->mode == 6) {
        memcpy(g_buf2, d2, 16);
        if ((res = send_fuse_cmd(g_buf, 16, 1))) return res;
        if ((res = send_cmd(g_buf, 16, num, 1))) return res;
        memcpy(d2, g_buf2, 16);
    }
    if (key) {
        memxor(d2, key, 16);
        memcpy(g_buf2, d2, 16);
        if ((res = send_cmd(g_buf, 16, num, 1))) return res;
        memcpy(d2, g_buf2, 16);
    }
    memcpy(hash, d2, 16);
    mac_init(c, 0);
    return 0;
}

static int cipher_init_decrypt(cipher_ctx *c, int mode, const uint8_t *iv, const uint8_t *key) {
    c->mode = mode;
    c->unkn = 1;
    memcpy(c->crypted, iv, 16);
    if (key) memxor(c->crypted, key, 16);
    return 0;
}

static int cipher_update(cipher_ctx *c, uint8_t *data, int aligned) {
    if (aligned == 0) return 0;
    if (aligned & 0xF) return -1025;
    static uint8_t kd[20 + 2048];
    int i = 0;
    while (aligned >= 2048) {
        int res = sub_0000(kd, data + i, 2048, c->crypted, &c->unkn, c->mode);
        aligned -= 2048;
        i += 2048;
        if (res) return res;
    }
    if (aligned == 0) return 0;
    return sub_0000(kd, data + i, aligned, c->crypted, &c->unkn, c->mode);
}

int mhfu_save_decrypt(int mode, uint8_t *data, int *len, const uint8_t key[16], const uint8_t expected[16]) {
    if (!g_kirk_ready) { kirk_init(&g_kirk); g_kirk_ready = 1; }
    int data_len = *len, aligned = (data_len + 15) & ~15;
    if (aligned <= 0x10) return -1;
    data_len -= 0x10;
    aligned -= 0x10;
    mac_ctx m;
    cipher_ctx c;
    if (mac_init(&m, mode) < 0) return -2;
    if (cipher_init_decrypt(&c, mode, data, key) < 0) return -3;
    if (mac_update(&m, data, 0x10) < 0) return -4;
    if (mac_update(&m, data + 0x10, aligned) < 0) return -5;
    if (cipher_update(&c, data + 0x10, aligned) < 0) return -6;
    if (expected) {
        uint8_t hash[16];
        if (mac_final(&m, hash, key) < 0) return -7;
        if (memcmp(hash, expected, 16)) return -8;
    }
    memmove(data, data + 0x10, (size_t)data_len);
    *len = data_len;
    return 0;
}
