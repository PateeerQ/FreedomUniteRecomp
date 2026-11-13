/* MHFU-Android -- texture packs; see texpack.h. */
#include "texpack.h"
#include "region.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif

#include <SDL3/SDL.h>

#include "psprecomp/mem.h"
#include "psprecomp/render.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

/* host/ktx2.cpp: a KTX2 (Basis Universal) file transcoded to what the GPU takes
 * (`want`: PSP_GL_ASTC4x4 | PSP_GL_BC7, else RGBA8), malloc'd; NULL if not. */
uint32_t *mhfu_ktx2_decode(const uint8_t *data, size_t size, int want, int *w, int *h, uint32_t *gl_fmt,
                           uint32_t *bytes);

/* ---- PPSSPP's StableQuickTexHash (GPU/Common/TextureDecoder.cpp) ----------------
 * The plain-C form PPSSPP keeps equal to its SSE2 and NEON ones (#7029): 16-byte
 * aligned data a multiple of 64 bytes long takes the vector path, the rest a
 * running sum. Guest memory is mapped 16-byte aligned, so the guest address
 * tells the alignment, as PPSSPP's host pointer does. */
static uint32_t quick_hash(const uint8_t *p, uint32_t guest, uint32_t size) {
    uint32_t check = 0;
    if ((guest & 0xF) == 0 && (size & 0x3F) == 0) {
        static const uint16_t init[8] = { 0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001 };
        uint16_t c16[8] = { 0 }, k[8];
        memcpy(k, init, sizeof k);
        for (uint32_t i = 0; i < size / 16; i += 4) {
            const uint8_t *b = p + i * 16;
            uint16_t a16[8], d16[8];
            uint32_t b32[4], e32[4], c32[4];
            memcpy(a16, b, 16);
            memcpy(b32, b + 16, 16);
            memcpy(e32, b + 32, 16);
            memcpy(d16, b + 48, 16);
            for (int j = 0; j < 8; j++) c16[j] = (uint16_t)(c16[j] + (uint16_t)(a16[j] * k[j]));
            memcpy(c32, c16, 16);
            for (int j = 0; j < 4; j++) { c32[j] ^= b32[j]; c32[j] += e32[j]; }
            memcpy(c16, c32, 16);
            for (int j = 0; j < 8; j++) c16[j] ^= (uint16_t)(d16[j] * k[j]);
            for (int j = 0; j < 8; j++) k[j] = (uint16_t)(k[j] + 0x2455);
        }
        uint32_t c32[4], k32[4];
        memcpy(c32, c16, 16);
        memcpy(k32, k, 16);
        for (int j = 0; j < 4; j++) c32[j] += k32[j];
        check = c32[0] + c32[1] + c32[2] + c32[3];
    } else {
        for (uint32_t i = 0; i < size / 8; i++) {
            uint32_t a, b;
            memcpy(&a, p + i * 8, 4);
            memcpy(&b, p + i * 8 + 4, 4);
            check += a;
            check ^= b;
        }
    }
    return check;
}

/* ---- xxHash (XXH32, XXH64), for packs made with "hash = xxh32" / "xxh64" ----------
 * The reference algorithms (Yann Collet, BSD-2); PPSSPP seeds both with
 * 0xBACD7814 and keeps the low 32 bits of XXH64. */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

static uint32_t xxh32(const uint8_t *p, size_t len, uint32_t seed) {
    const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    const uint8_t *end = p + len;
    uint32_t h;
    if (len >= 16) {
        uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t *limit = end - 16;
        do {
            v1 = rotl32(v1 + rd32(p) * P2, 13) * P1; p += 4;
            v2 = rotl32(v2 + rd32(p) * P2, 13) * P1; p += 4;
            v3 = rotl32(v3 + rd32(p) * P2, 13) * P1; p += 4;
            v4 = rotl32(v4 + rd32(p) * P2, 13) * P1; p += 4;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P5;
    }
    h += (uint32_t)len;
    for (; p + 4 <= end; p += 4) h = rotl32(h + rd32(p) * P3, 17) * P4;
    for (; p < end; p++) h = rotl32(h + *p * P5, 11) * P1;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

static uint64_t xxh64_round(uint64_t acc, uint64_t in) {
    acc += in * 14029467366897019727ull;
    return rotl64(acc, 31) * 11400714785074694791ull;
}
static uint64_t xxh64_merge(uint64_t acc, uint64_t v) {
    acc ^= xxh64_round(0, v);
    return acc * 11400714785074694791ull + 9650029242287828579ull;
}
static uint64_t xxh64(const uint8_t *p, size_t len, uint64_t seed) {
    const uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                   P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t *limit = end - 32;
        do {
            v1 = xxh64_round(v1, rd64(p)); p += 8;
            v2 = xxh64_round(v2, rd64(p)); p += 8;
            v3 = xxh64_round(v3, rd64(p)); p += 8;
            v4 = xxh64_round(v4, rd64(p)); p += 8;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xxh64_merge(h, v1); h = xxh64_merge(h, v2); h = xxh64_merge(h, v3); h = xxh64_merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += (uint64_t)len;
    for (; p + 8 <= end; p += 8) h = rotl64(h ^ xxh64_round(0, rd64(p)), 27) * P1 + P4;
    if (p + 4 <= end) { h = rotl64(h ^ ((uint64_t)rd32(p) * P1), 23) * P2 + P3; p += 4; }
    for (; p < end; p++) h = rotl64(h ^ (*p * P5), 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

enum { HASH_QUICK, HASH_XXH32, HASH_XXH64 };
static int g_hash = HASH_QUICK;                         /* the pack's [options] hash */
static int g_ignore_addr;                               /* [options] ignoreAddress (not with the quick hash) */

static uint32_t data_hash(const uint8_t *p, uint32_t guest, uint32_t size) {
    switch (g_hash) {
    case HASH_XXH32: return xxh32(p, size, 0xBACD7814u);
    case HASH_XXH64: return (uint32_t)xxh64(p, size, 0xBACD7814u);
    default:         return quick_hash(p, guest, size);
    }
}

static const int BITS[8] = { 16, 16, 16, 32, 4, 8, 16, 32 };   /* PSP_TPSM_5650 .. T32 */

/* TextureReplacer::ComputeHash, for the "quick" hash: the bytes the texture
 * covers (bufw-wide rows), or row by row when the buffer is wider. 0 if the
 * texture is not all in memory. */
static uint32_t texture_hash(uint32_t addr, int bufw, int w, int h, int fmt) {
    const uint32_t bits = (uint32_t)BITS[fmt & 7];
    if (bufw <= w) {
        const uint32_t pixels = (uint32_t)(bufw * h + (w - bufw));
        const uint32_t size = bits * pixels / 8;
        const uint8_t *p = (const uint8_t *)psp_mem_ptr(addr, size);
        return p ? data_hash(p, addr, size) : 0;
    }
    const uint32_t line = bits * (uint32_t)w / 8, stride = bits * (uint32_t)bufw / 8;
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(addr, (uint32_t)(h - 1) * stride + line);
    if (!p) return 0;
    uint32_t r = 0;
    for (int y = 0; y < h; y++) r = (r * 11) ^ data_hash(p + (uint32_t)y * stride, addr + (uint32_t)y * stride, line);
    return r;
}

/* ---- the pack ------------------------------------------------------------------- */

enum { F_UNLOADED, F_LOADING, F_READY, F_FAILED };

typedef struct {
    char *path;
    int state;
    int w, h;
    uint32_t *rgba;              /* F_READY: the picture, kept until the budget needs it */
    uint32_t gl_fmt, bytes;      /* its form: 0 for RGBA8, else a compressed GL format; its size */
    uint64_t used;               /* when it was last handed out (for the budget) */
} pfile;

typedef struct { uint64_t ckey; uint32_t hash; int file; } pentry;

static pentry *g_tab;            /* open addressing on the hash */
static uint32_t g_cap;
static pfile *g_files;
static int g_nfiles;
static char g_dir[1024];                                /* where textures.ini is */
/* Packs shipped as MAIN/ + ADDONS/<style>/ (MH2G_KHD: button styles, an
 * ultrawide HUD, the vanilla HUD): a style's files take the place of MAIN's
 * of the same path. g_pack is the folder above MAIN, g_addon the style. */
static char g_pack[1024], g_addon[256];
static char *g_addons[32];
static int g_naddons;
static SDL_Mutex *g_lock;
static SDL_Condition *g_wake;
static int *g_queue, g_nq;
static uint64_t g_clock, g_bytes, g_budget;
static int g_hits, g_misses, g_loaded, g_failed;
static int g_ntex;                                      /* textures listed by the pack in use */

static uint32_t mix(uint64_t ckey, uint32_t hash) {
    uint64_t x = ckey ^ ((uint64_t)hash * 0x9E3779B97F4A7C15ull);
    x ^= x >> 31;
    return (uint32_t)(x * 0xBF58476D1CE4E5B9ull >> 32);
}

static int find(uint64_t ckey, uint32_t hash) {
    if (!g_cap) return -1;
    for (uint32_t i = mix(ckey, hash) & (g_cap - 1), n = 0; n < g_cap; i = (i + 1) & (g_cap - 1), n++) {
        if (g_tab[i].file < 0) return -1;
        if (g_tab[i].ckey == ckey && g_tab[i].hash == hash) return g_tab[i].file;
    }
    return -1;
}

static void insert(uint64_t ckey, uint32_t hash, int file) {
    uint32_t i = mix(ckey, hash) & (g_cap - 1);
    while (g_tab[i].file >= 0 && !(g_tab[i].ckey == ckey && g_tab[i].hash == hash)) i = (i + 1) & (g_cap - 1);
    g_tab[i].ckey = ckey;
    g_tab[i].hash = hash;
    g_tab[i].file = file;
}

static int file_index(const char *rel) {
    for (int i = g_nfiles - 1; i >= 0 && i >= g_nfiles - 4; i--)       /* repeats come together */
        if (!strcmp(g_files[i].path, rel)) return i;
    static int cap;
    if (g_nfiles == cap) {
        cap = cap ? cap * 2 : 1024;
        g_files = (pfile *)realloc(g_files, sizeof *g_files * (size_t)cap);
    }
    memset(&g_files[g_nfiles], 0, sizeof g_files[0]);
    g_files[g_nfiles].path = strdup(rel);
    return g_nfiles++;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

static int ini_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int count = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) if (strchr(line, '=')) count++;
    fclose(f);
    return count;
}

/* [games] <game id> = <another ini>: PPSSPP's per-game override, read after
 * textures.ini (its keys win). "" if none. */
static void ini_games(const char *path, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[2048];
    int in_games = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '[') { in_games = !SDL_strncasecmp(s, "[games]", 7); continue; }
        char *eq = in_games ? strchr(s, '=') : NULL;
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!SDL_strcasecmp(key, MHFU_GAME_ID) && *val && strcmp(val, "true") && SDL_strcasecmp(val, "textures.ini") &&
            !strstr(val, ".."))
            snprintf(out, cap, "%s", val);
    }
    fclose(f);
}

/* The [hashes] section: "<key> = <file>[|mip files]". Others are left alone. */
static int ini_parse(const char *path, int *with_clut, int *with_addr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int in_hashes = 0;
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') { in_hashes = !SDL_strncasecmp(s, "[hashes]", 8); continue; }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!in_hashes) {
            if (!SDL_strcasecmp(key, "hash"))
                g_hash = !SDL_strcasecmp(val, "xxh32") ? HASH_XXH32 : !SDL_strcasecmp(val, "xxh64") ? HASH_XXH64
                                                                                                     : HASH_QUICK;
            else if (!SDL_strcasecmp(key, "ignoreAddress"))
                g_ignore_addr = !SDL_strcasecmp(val, "true") || !strcmp(val, "1");
            continue;
        }
        if (strchr(key, '_')) continue;                 /* a mip level of its own: the base is enough */
        unsigned long long ckey = 0;
        unsigned hash = 0;
        if (strlen(key) < 24 || sscanf(key, "%16llx%8x", &ckey, &hash) != 2) continue;
        char *bar = strchr(val, '|');
        if (bar) *bar = 0;
        val = trim(val);
        if (!*val) continue;                            /* an ignored texture */
        for (char *c = val; *c; c++) if (*c == '\\') *c = '/';
        insert(ckey, hash, file_index(val));
        n++;
        if (ckey & 0xFFFFFFFFull) (*with_clut)++;
        if (ckey >> 32) (*with_addr)++;
    }
    fclose(f);
    return n;
}

static int read_ini(const char *path) {
    char over[512], opath[1600] = "";
    ini_games(path, over, sizeof over);
    if (over[0]) snprintf(opath, sizeof opath, "%s/%s", g_dir, over);
    /* A table twice as large as both lists. */
    const int count = ini_lines(path) + (opath[0] ? ini_lines(opath) : 0);
    g_cap = 16;
    while (g_cap < (uint32_t)count * 2 + 16) g_cap <<= 1;
    g_tab = (pentry *)malloc(sizeof *g_tab * g_cap);
    for (uint32_t i = 0; i < g_cap; i++) g_tab[i].file = -1;
    int with_clut = 0, with_addr = 0;
    int n = ini_parse(path, &with_clut, &with_addr);
    if (n < 0) return -1;
    if (opath[0]) {
        const int m = ini_parse(opath, &with_clut, &with_addr);
        printf("texpack: [games] %s = %s: %d more\n", MHFU_GAME_ID, over, m);
        if (m > 0) n += m;
    }
    if (g_ignore_addr && g_hash == HASH_QUICK) g_ignore_addr = 0;   /* as PPSSPP: needs a safer hash */
    printf("texpack: %s hash%s\n", g_hash == HASH_XXH32 ? "xxh32" : g_hash == HASH_XXH64 ? "xxh64" : "quick",
           g_ignore_addr ? ", ignoreAddress" : "");
    /* Issue #14's pack lists its title and button icons by address and CLUT hash. */
    if (with_clut || with_addr) printf("texpack: %d keys with a CLUT hash, %d with an address\n", with_clut, with_addr);
    return n;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* ---- loading -------------------------------------------------------------------- */

#ifndef _WIN32
/* PPSSPP reads a pack's files ignoring case (its DirectoryReader), and packs
 * made on Windows name them as they like (issue #14 on Linux: "could not read
 * allarmorhdmale/....PNG"). Each part of the path matched without case. */
static FILE *open_nocase(const char *path) {
    char out[2048], part[512];
    const char *p = path;
    size_t o = 0;
    if (*p == '/') { out[o++] = '/'; p++; }
    out[o] = 0;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof part) return NULL;
        memcpy(part, p, len);
        part[len] = 0;
        p += len + (e ? 1 : 0);
        if (!len) continue;
        DIR *d = opendir(o ? out : ".");
        if (!d) return NULL;
        char hit[512];
        int found = 0;                                  /* 2: exact, 1: without case */
        struct dirent *de;
        while (found < 2 && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, part)) found = 2;
            else if (!found && !strcasecmp(de->d_name, part)) {
                snprintf(hit, sizeof hit, "%s", de->d_name);
                found = 1;
            }
        }
        closedir(d);
        if (!found) return NULL;
        if (found == 1) snprintf(part, sizeof part, "%s", hit);
        const size_t pl = strlen(part);
        if (o + pl + 2 >= sizeof out) return NULL;
        if (o && out[o - 1] != '/') out[o++] = '/';
        memcpy(out + o, part, pl + 1);
        o += pl;
    }
    return fopen(out, "rb");
}
#endif

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
#ifndef _WIN32
    if (!f) f = open_nocase(path);
#endif
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = b ? (size_t)len : 0;
    return b;
}

static uint32_t *decode(const char *rel, int *w, int *h, uint32_t *gl_fmt, uint32_t *bytes) {
    char path[2048];
    size_t n = 0;
    uint8_t *data = NULL;
    if (g_addon[0]) {                                   /* the chosen style's picture, where it has one */
        snprintf(path, sizeof path, "%s/ADDONS/%s/%s", g_pack, g_addon, rel);
        data = read_all(path, &n);
    }
    if (!data) {
        snprintf(path, sizeof path, "%s/%s", g_dir, rel);
        data = read_all(path, &n);
    }
    if (!data) return NULL;
    uint32_t *px = NULL;
    const size_t len = strlen(rel);
    *gl_fmt = 0;
    if (len > 5 && !SDL_strcasecmp(rel + len - 5, ".ktx2")) {
        /* What the GPU takes compressed; RGBA8 if neither (or the GL is not up yet). */
        int want = psp_render_gl_compressed_formats();
        if (want < 0) want = 0;
        if (getenv("MHFU_TEXPACK_RGBA")) want = 0;      /* for comparing */
        px = mhfu_ktx2_decode(data, n, want, w, h, gl_fmt, bytes);
    } else {
        int comp = 0;
        px = (uint32_t *)stbi_load_from_memory(data, (int)n, w, h, &comp, 4);
        if (px) *bytes = (uint32_t)*w * (uint32_t)*h * 4;
    }
    free(data);
    return px;
}

/* Pictures past the budget go, oldest first (they load again when wanted). */
static void trim_budget(void) {
    while (g_bytes > g_budget) {
        int old = -1;
        for (int i = 0; i < g_nfiles; i++)
            if (g_files[i].state == F_READY && (old < 0 || g_files[i].used < g_files[old].used)) old = i;
        if (old < 0) break;
        g_bytes -= g_files[old].bytes;
        free(g_files[old].rgba);
        g_files[old].rgba = NULL;
        g_files[old].state = F_UNLOADED;
    }
}

static int SDLCALL loader(void *arg) {
    (void)arg;
    SDL_LockMutex(g_lock);
    for (;;) {
        while (!g_nq) SDL_WaitCondition(g_wake, g_lock);
        const int fi = g_queue[--g_nq];                 /* newest first: what is on screen now */
        char rel[1024];
        snprintf(rel, sizeof rel, "%s", g_files[fi].path);
        SDL_UnlockMutex(g_lock);
        int w = 0, h = 0;
        uint32_t gl_fmt = 0, bytes = 0;
        uint32_t *px = decode(rel, &w, &h, &gl_fmt, &bytes);
        SDL_LockMutex(g_lock);
        if (px) {
            g_files[fi].rgba = px;
            g_files[fi].w = w;
            g_files[fi].h = h;
            g_files[fi].gl_fmt = gl_fmt;
            g_files[fi].bytes = bytes;
            g_files[fi].state = F_READY;
            g_files[fi].used = ++g_clock;
            g_bytes += bytes;
            g_loaded++;
            trim_budget();
        } else {
            g_files[fi].state = F_FAILED;
            g_failed++;
            printf("texpack: could not read %s\n", rel);
            fflush(stdout);
        }
    }
    return 0;
}

/* psprecomp's replacer: on the GE's thread, at every texture decode. */
/* TextureReplacer's LookupWildcard: the full key, then with parts zeroed. */
static int lookup(uint64_t ck, uint32_t hash) {
    const uint64_t lo = ck & 0xFFFFFFFFull, hi = ck & ~0xFFFFFFFFull;
    int fi;
    if ((fi = find(ck, hash)) >= 0) return fi;
    if ((fi = find(lo, 0)) >= 0) return fi;             /* the CLUT hash only */
    if (!g_ignore_addr && (fi = find(ck, 0)) >= 0) return fi;
    if ((fi = find(lo, hash)) >= 0) return fi;          /* no address */
    if (!g_ignore_addr && (fi = find(hi, hash)) >= 0) return fi;   /* no CLUT hash */
    if (!g_ignore_addr && (fi = find(hi, 0)) >= 0) return fi;      /* the address only */
    return find(0, hash);                               /* the data alone */
}

static int replace(uint32_t addr, int bufw, int w, int h, int fmt, int swizzled, uint32_t dim, const uint8_t *clut,
                   uint32_t clut_bytes, int *ow, int *oh, uint32_t **rgba, uint32_t *gl_fmt, uint32_t *bytes) {
    (void)swizzled;                                     /* PPSSPP hashes swizzled ones as they lie, too */
    if (fmt < 0 || fmt > 7 || w <= 0 || h <= 0 || bufw <= 0) return PSP_REPLACE_NONE;
    const uint32_t hash = texture_hash(addr, bufw, w, h, fmt);
    /* PPSSPP's key (TexCacheEntry::CacheKey): the address above, TSIZE's log2
     * sizes below, XORed with the CLUT's hash for a paletted texture (XXH32 of
     * the CLUT bytes, seed 0xC0108888, as its replacer has it). Issue #14: a
     * pack listing the title and button icons by address and CLUT. */
    uint64_t ck = (uint64_t)(addr & 0x3FFFFFFFu) << 32 | (dim & 0xF0Fu);
    if ((fmt & 4) && clut) ck ^= xxh32(clut, clut_bytes, 0xC0108888u);
    if (g_ignore_addr) ck &= 0xFFFFFFFFull;
    int fi = lookup(ck, hash);
    if (fi < 0) {
        g_misses++;
        /* MHFU_TEXPACK_LOG=1: each texture the pack lacks, once, in the ini's key form. */
        static int log = -1, nseen;
        static uint32_t seen[256];
        if (log < 0) { const char *e = getenv("MHFU_TEXPACK_LOG"); log = e && *e && *e != '0'; }
        if (log && nseen < 256) {
            int have = 0;
            for (int i = 0; i < nseen; i++) have |= seen[i] == hash;
            if (!have) {
                seen[nseen++] = hash;
                printf("texpack: not in the pack: %016llx%08x (%dx%d, format %d, at 0x%08X)\n",
                       (unsigned long long)ck, hash, w, h, fmt, addr);
                fflush(stdout);
            }
        }
        return PSP_REPLACE_NONE;
    }
    int r = PSP_REPLACE_NONE;
    SDL_LockMutex(g_lock);
    pfile *f = &g_files[fi];
    switch (f->state) {
    case F_READY: {
        uint32_t *copy = (uint32_t *)malloc(f->bytes);
        if (copy) {
            memcpy(copy, f->rgba, f->bytes);
            *ow = f->w;
            *oh = f->h;
            *rgba = copy;
            *gl_fmt = f->gl_fmt;
            *bytes = f->bytes;
            f->used = ++g_clock;
            g_hits++;
            r = PSP_REPLACE_READY;
        }
        break;
    }
    case F_UNLOADED:
        f->state = F_LOADING;
        g_queue[g_nq++] = fi;
        SDL_SignalCondition(g_wake);
        /* fall through */
    case F_LOADING:
        r = PSP_REPLACE_PENDING;
        break;
    default:
        break;                                          /* F_FAILED: the game's own */
    }
    SDL_UnlockMutex(g_lock);
    return r;
}

/* ---- styles (ADDONS) ------------------------------------------------------------ */

static int is_dir(const char *p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

typedef struct { char *name[64]; int n; } names;
static SDL_EnumerationResult SDLCALL collect(void *ud, const char *dir, const char *name) {
    names *ns = (names *)ud;
    char p[2048];
    snprintf(p, sizeof p, "%s%s", dir, name);
    if (ns->n < 64 && name[0] != '.' && is_dir(p)) ns->name[ns->n++] = strdup(name);
    return SDL_ENUM_CONTINUE;
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* A style is a folder under ADDONS that holds one of MAIN's own folders (ui/...). */
static void scan_addons(const char *rel, int depth) {
    char p[2048];
    snprintf(p, sizeof p, "%s/ADDONS%s%s", g_pack, rel[0] ? "/" : "", rel);
    names ns = { { 0 }, 0 };
    SDL_EnumerateDirectory(p, collect, &ns);
    qsort(ns.name, (size_t)ns.n, sizeof ns.name[0], by_name);
    int style = 0;
    for (int i = 0; i < ns.n && rel[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", g_dir, ns.name[i]);
        if (is_dir(p)) style = 1;
    }
    if (style && g_naddons < 32) g_addons[g_naddons++] = strdup(rel);
    for (int i = 0; i < ns.n; i++) {
        if (!style && depth < 4) {
            char sub[512];
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", ns.name[i]);
            scan_addons(sub, depth + 1);
        }
        free(ns.name[i]);
    }
}

int mhfu_texpack_styles(void) { return g_naddons; }
const char *mhfu_texpack_style(int i) { return i >= 0 && i < g_naddons ? g_addons[i] : ""; }
const char *mhfu_texpack_style_in_use(void) { return g_addon; }

int mhfu_texpack_init(const char *root) {
    const char *env = getenv("MHFU_TEXPACK");
    if (env && !strcmp(env, "0")) return -1;           /* MHFU_TEXPACK=0: off */
    char dir[1024];
    if (env && env[0]) snprintf(dir, sizeof dir, "%s", env);
    else {
        /* <root>/textures/<game id>; else another release's folder (MHP2G packs
         * carry the FU lists too, and a pack is easily put under the wrong id). */
        static const char *const ids[] = { MHFU_GAME_ID, "ULES01213", "ULUS10391", "ULJM05500" };
        for (int i = 0; i < 4; i++) {
            snprintf(dir, sizeof dir, "%s/textures/%s", root, ids[i]);
            char p1[1100], p2[1100];
            snprintf(p1, sizeof p1, "%s/textures.ini", dir);
            snprintf(p2, sizeof p2, "%s/MAIN/textures.ini", dir);
            if (exists(p1) || exists(p2)) break;
            snprintf(dir, sizeof dir, "%s/textures/%s", root, MHFU_GAME_ID);
        }
    }
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/MAIN/textures.ini", dir);
    if (exists(probe)) snprintf(g_dir, sizeof g_dir, "%s/MAIN", dir);
    else snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(g_pack, sizeof g_pack, "%s", dir);

    /* The release's own list first (packs made for MHP2G carry FU ones too). */
    const char *mine = !strcmp(MHFU_REGION_TAG, "EU") ? "textures_FU_EU.ini"
                     : !strcmp(MHFU_REGION_TAG, "US") ? "textures_FU_US.ini" : NULL;
    char ini[1100] = "";
    if (mine) snprintf(ini, sizeof ini, "%s/%s", g_dir, mine);
    if (!mine || !exists(ini)) snprintf(ini, sizeof ini, "%s/textures.ini", g_dir);
    if (!exists(ini)) {
        if (env && env[0]) printf("texpack: no textures.ini in %s\n", g_dir);
        return -1;
    }
    const int n = read_ini(ini);
    if (n <= 0) { printf("texpack: %s lists no textures\n", ini); return -1; }
    g_lock = SDL_CreateMutex();
    g_wake = SDL_CreateCondition();
    g_queue = (int *)malloc(sizeof *g_queue * (size_t)g_nfiles);
    const char *mb = getenv("MHFU_TEXPACK_MB");
#ifdef __ANDROID__
    g_budget = (uint64_t)(mb ? atoi(mb) : 256) << 20;
#else
    g_budget = (uint64_t)(mb ? atoi(mb) : 768) << 20;
#endif
    if (!g_lock || !g_wake || !g_queue || !SDL_CreateThread(loader, "texpack", NULL)) {
        printf("texpack: could not start its loader\n");
        return -1;
    }
    scan_addons("", 0);
    const char *style = getenv("MHFU_TEXPACK_STYLE");
    for (int i = 0; style && i < g_naddons; i++)
        if (!strcmp(style, g_addons[i])) snprintf(g_addon, sizeof g_addon, "%s", style);
    if (g_naddons) printf("texpack: %d styles in ADDONS, %s in use\n", g_naddons, g_addon[0] ? g_addon : "MAIN's own");
    psp_render_gl_set_replacer(replace);
    g_ntex = n;
    printf("texpack: %d textures (%d files) from %s, %llu MB kept in memory at most\n", n, g_nfiles, ini,
           (unsigned long long)(g_budget >> 20));
    fflush(stdout);
    return 0;
}

int mhfu_texpack_textures(void) { return g_ntex; }

void mhfu_texpack_report(void) {
    if (!g_nfiles) return;
    printf("texpack: %d replaced uploads, %d textures not in the pack, %d files loaded, %d unreadable\n",
           g_hits, g_misses, g_loaded, g_failed);
}
