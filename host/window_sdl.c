/* MHFU-Android -- the window through SDL3: window.h for Android and Linux,
 * and for Windows when built with MHFU_SDL_WINDOW (window.c is the Win32 one,
 * with the Multiplayer menu and dialogs this one does not have yet).
 *
 * SDL wants its window and events on the program's main thread (on Android
 * it insists), so mhfu_window_main runs SDL there and the game on a thread of
 * its own. The game's calls here (mhfu_window_start, _gl, _ask_text, _notify)
 * hand their work to that loop.
 *
 * The context is GL 3.3 core on the desktop and GLES 3.0 on Android
 * (MHFU_GLES=1 asks for GLES on the desktop too, to try the GLES shaders).
 * psprecomp's render thread creates and uses it. The overlay -- the FPS line,
 * achievement toasts, the text box for the game's keyboard -- is drawn here
 * into an image with stb_truetype and handed to psp_render_gl_overlay. */
#include "window.h"
#include "region.h"
#include "achievements.h"
#include "about.h"

#if MHFU_SDL_WINDOW

#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "gamepad.h"
#include "hle_mhfu.h"
#include "adhoc.h"
#include "chat.h"
#include "input.h"
#include "keys.h"
#include "lobby.h"
#include "mods.h"
#include "net/client.h"
#include "perfhint.h"
#include "stats.h"
#if MHFU_TEXPACK
#include "texpack.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#define PSP_W 480
#define PSP_H 272

static SDL_Window      *g_window;
static SDL_GLContext    g_ctx;
static int              g_scale = 2, g_fullscreen, g_out_w, g_out_h, g_gles;
static SDL_AtomicInt    g_buttons, g_stick, g_closed;
static void           (*g_on_close)(void);
static int              g_gl;
static float            g_ui = 1;               /* the display's scale, for the overlay's sizes */
static Uint32           g_ev_window, g_ev_wake;   /* our own event types */
static int              g_show_fps = 1, g_show_fps_init = 1;
static SDL_Mutex       *g_lock;
static SDL_Condition   *g_cond;
static int              g_window_state;          /* 0 asked for, 1 made, -1 failed */

/* ---- text: stb_truetype over a system font ---------------------------------------- */

static stbtt_fontinfo g_font;
static unsigned char *g_font_data;
static int            g_font_ok;

static void font_init(void) {
    static const char *const candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0] && !g_font_ok; i++) {
        size_t n = 0;
        void *d = SDL_LoadFile(candidates[i], &n);
        if (!d) continue;
        if (stbtt_InitFont(&g_font, (const unsigned char *)d, stbtt_GetFontOffsetForIndex((const unsigned char *)d, 0))) {
            g_font_data = (unsigned char *)d;
            g_font_ok = 1;
        } else SDL_free(d);
    }
    if (!g_font_ok) fprintf(stderr, "window: no system font found; the overlay has no text\n");
}

typedef struct { int cp, px, w, h, xoff, yoff, adv; unsigned char *bmp; } glyph;
#define GLYPHS 768
static glyph g_glyphs[GLYPHS];
static int   g_nglyphs;

static const glyph *glyph_of(int cp, int px) {
    for (int i = 0; i < g_nglyphs; i++) if (g_glyphs[i].cp == cp && g_glyphs[i].px == px) return &g_glyphs[i];
    if (g_nglyphs == GLYPHS) {                       /* full: start over */
        for (int i = 0; i < g_nglyphs; i++) stbtt_FreeBitmap(g_glyphs[i].bmp, NULL);
        g_nglyphs = 0;
    }
    glyph *g = &g_glyphs[g_nglyphs++];
    const float s = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    g->cp = cp;
    g->px = px;
    g->adv = (int)lroundf(adv * s);
    g->bmp = stbtt_GetCodepointBitmap(&g_font, s, s, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* The next code point of a UTF-8 string. */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s++, n = 0;
    if (c >= 0xF0) { c &= 0x07; n = 3; } else if (c >= 0xE0) { c &= 0x0F; n = 2; } else if (c >= 0xC0) { c &= 0x1F; n = 1; }
    while (n-- && (*s & 0xC0) == 0x80) c = c << 6 | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}

static int text_width(const char *t, int px) {
    if (!g_font_ok) return 0;
    int w = 0;
    while (*t) w += glyph_of(utf8_next(&t), px)->adv;
    return w;
}

/* ---- the overlay canvas: 0xAABBGGRR, what psp_render_gl_overlay takes -------------- */

typedef struct { uint32_t *px; int w, h; } canvas;

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static int g_clip0, g_clip1 = 1 << 30;     /* rows drawn: [g_clip0, g_clip1) */

/* Source-over, on a canvas whose pixels may still be transparent. */
static void blend(canvas *c, int x, int y, uint32_t col, int cov) {
    if (x < 0 || y < g_clip0 || x >= c->w || y >= c->h || y >= g_clip1) return;
    const int sa = (int)(col >> 24) * cov / 255;
    if (!sa) return;
    uint32_t *d = &c->px[y * c->w + x];
    const int da = (int)(*d >> 24), oa = sa + da * (255 - sa) / 255;
    if (!oa) return;
    int out = 0;
    for (int k = 0; k < 24; k += 8) {
        const int s = (int)(col >> k & 0xFF), dd = (int)(*d >> k & 0xFF);
        out |= ((s * sa + dd * da * (255 - sa) / 255) / oa) << k;
    }
    *d = (uint32_t)out | (uint32_t)oa << 24;
}

static void fill(canvas *c, int x, int y, int w, int h, uint32_t col) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) blend(c, i, j, col, 255);
}

static void frame_rect(canvas *c, int x, int y, int w, int h, int t, uint32_t col) {
    fill(c, x, y, w, t, col); fill(c, x, y + h - t, w, t, col);
    fill(c, x, y, t, h, col); fill(c, x + w - t, y, t, h, col);
}

static void diamond(canvas *c, int cx, int cy, int r, uint32_t col) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (abs(i) + abs(j) <= r) blend(c, cx + i, cy + j, col, 255);
}

/* Text on a baseline, cut with "..." at max_w (0: no limit). */
static void text(canvas *c, int x, int baseline, int px, uint32_t col, const char *t, int max_w) {
    if (!g_font_ok) return;
    const int dots = max_w ? text_width("...", px) : 0;
    const int whole = text_width(t, px);
    int pen = x;
    while (*t) {
        const char *before = t;
        const glyph *g = glyph_of(utf8_next(&t), px);
        if (max_w && whole > max_w && pen + g->adv > x + max_w - dots) { t = before; break; }
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) blend(c, pen + g->xoff + i, baseline + g->yoff + j, col, g->bmp[j * g->w + i]);
        pen += g->adv;
    }
    if (*t) text(c, pen, baseline, px, col, "...", 0);
}

/* Up to `lines` lines, broken at spaces. */
static void text_wrapped(canvas *c, int x, int baseline, int px, int line_h, uint32_t col, const char *t, int max_w, int lines) {
    char line[512];
    for (int l = 0; l < lines && *t; l++) {
        int n = 0, last_space = -1;
        const char *p = t;
        while (*p) {
            const char *q = p;
            utf8_next(&q);
            if ((size_t)(n + (q - p)) >= sizeof line) break;
            memcpy(line + n, p, (size_t)(q - p));
            line[n + (q - p)] = 0;
            if (text_width(line, px) > max_w) break;
            if (*p == ' ') last_space = n;
            n += (int)(q - p);
            p = q;
        }
        if (*p && last_space > 0 && l + 1 < lines) { n = last_space; p = t + n + 1; }
        line[n] = 0;
        text(c, x, baseline + l * line_h, px, col, l + 1 == lines && *p ? t : line, max_w);
        t = p;
    }
}

/* Anti-aliased shapes: coverage from the distance to the edge. */
static int cov(float d) { return d <= -0.5f ? 255 : d >= 0.5f ? 0 : (int)((0.5f - d) * 255); }

static void disc(canvas *c, float cx, float cy, float r, uint32_t col) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
            const int k = cov(d);
            if (k) blend(c, x, y, col, k);
        }
}

static void ring(canvas *c, float cx, float cy, float r, float t, uint32_t col) {
    for (int y = (int)(cy - r - t); y <= (int)(cy + r + t); y++)
        for (int x = (int)(cx - r - t); x <= (int)(cx + r + t); x++) {
            const float d = fabsf(sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r) - t / 2;
            const int k = cov(d);
            if (k) blend(c, x, y, col, k);
        }
}

static void segment(canvas *c, float x0, float y0, float x1, float y1, float t, uint32_t col) {
    const float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t, miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
    const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++)
        for (int x = (int)minx; x <= (int)maxx; x++) {
            const float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float u = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            u = u < 0 ? 0 : u > 1 ? 1 : u;
            const float ex = px - u * dx, ey = py - u * dy;
            const int k = cov(sqrtf(ex * ex + ey * ey) - t / 2);
            if (k) blend(c, x, y, col, k);
        }
}

static void round_rect(canvas *c, float x, float y, float w, float h, float r, uint32_t col, float border, uint32_t bcol) {
    for (int j = (int)y; j < (int)(y + h); j++)
        for (int i = (int)x; i < (int)(x + w); i++) {
            const float qx = fmaxf(fabsf(i + 0.5f - (x + w / 2)) - (w / 2 - r), 0);
            const float qy = fmaxf(fabsf(j + 0.5f - (y + h / 2)) - (h / 2 - r), 0);
            const float d = sqrtf(qx * qx + qy * qy) - r;
            const int k = cov(d);
            if (!k) continue;
            const int inner = border > 0 ? cov(d + border) : 255;
            if (inner) blend(c, i, j, col, inner * k / 255);
            if (border > 0 && k > inner) blend(c, i, j, bcol, k - inner);
        }
}

static void text_centered(canvas *c, float cx, float cy, int px, uint32_t col, const char *t) {
    text(c, (int)(cx - text_width(t, px) / 2.0f), (int)(cy + px * 0.36f), px, col, t, 0);
}

/* ---- touch controls ------------------------------------------------------------------
 *
 * A PSP laid over the screen in groups: the analog stick, the d-pad, the face
 * buttons, L, R, Select, Start and the menu button. Each group has a centre
 * and a size; the defaults put the d-pad and stick on the left, the face
 * buttons on the right, L and R in the top corners, Select and Start at the
 * bottom and the menu button at the top. Sizes are in units of the screen's
 * height, so they stay round on any phone. The layout editor (the drawer's
 * "Edit button layout") moves and resizes each group; positions are kept as
 * fractions of the screen, with the opacity and the other settings, in
 * settings.ini in SDL's preferences folder.
 *
 * Every finger is tracked: the stick follows its finger, the d-pad takes 8
 * directions, and a finger slides from button to button. The drawer turns the
 * controls on, off or automatic -- off while a controller is connected. */

enum { T_NONE, T_STICK, T_DPAD, T_CROSS, T_CIRCLE, T_SQUARE, T_TRIANGLE, T_L, T_R, T_SELECT, T_START, T_MENU, T_EDGE, T_RSTICK, T_SWIPE,
       T_SKILL0, T_SKILL1, T_SKILL2, T_ATTACK, T_ROLL, T_ITEM, T_RUN, T_YES, T_NO, T_ACT, T_ACT2, T_DETAIL, T_LLEFT, T_LRIGHT, T_CHAT,
       T_DRAW, T_USE, T_KICK, T_BAG, T_PICK, T_QSTART, T_WX0, T_WX1, T_WX2, T_WX3, T_WX4, T_WX5 };
enum { VPAD_AUTO, VPAD_ON, VPAD_OFF };
enum { G_STICK, G_DPAD, G_FACE, G_L, G_R, G_SELECT, G_START, G_MENU, G_RSTICK, G_SKILL, G_ATTACK, G_ROLL, G_ITEM, G_RUN,
       G_DRAW, G_USE, G_KICK, G_BAG, G_YESNO, G_EXTRAS, G_ACT, G_PICK, G_COUNT };
static const char *const k_group_name[G_COUNT] = { "stick", "dpad", "face", "l", "r", "select", "start", "menu", "rstick", "skill",
                                                   "attack", "roll", "item", "run",
                                                   "draw", "use", "kick", "bag", "yesno", "extras", "act", "pick" };
/* The button styles, chosen in the drawer: the PSP's face buttons and d-pad
 * everywhere, or the mobile one: in a quest with the weapon out, the attack
 * pad, Dodge (Cross), Sheathe (Square) and the weapon buttons (R is on the pad
 * and on them); the PSP's buttons everywhere else (sheathed, the village, the
 * menus: no flag for an open menu or the game's prompts was found). The stick,
 * L, Select and Start in all. */
enum { STYLE_PSP, STYLE_MOBILE };
enum { CTX_FREE, CTX_SHEATHED, CTX_DRAWN };

static int     g_vpad_mode = VPAD_AUTO;
static int     g_vpad_alpha = 100;                    /* percent */
static int     g_skills_on = 1;                        /* the weapon buttons */
static int     g_style = STYLE_MOBILE;                  /* the button style */
/* The stick and the d-pad as one round control (the default): the stick in the
 * middle, the d-pad a ring of four arcs around it (8 ways; a finger slides
 * round it). 0: the two apart, as before. */
static int     g_pad_combined = 1;
#define PAD_INNER 0.62f                                  /* the stick's part of the radius */
static float   g_attack_kx, g_attack_ky;                /* the attack pad's knob, -1..1 */
static int     g_attack_held;
static int     g_menu_open;
static int     g_edit;                                 /* the layout editor is open */
static int     g_edit_sel = -1;                        /* the group being edited */
static uint32_t g_touch_buttons;
static int     g_skill_down;                            /* the weapon buttons held, a bit each */
static int     g_touch_ax = -1, g_touch_ay = -1;         /* the stick, 0-255; -1 not touched */
static float   g_touch_rx, g_touch_ry;                  /* where the camera swipe went down */
static int     g_touch_rheld;                           /* a camera swipe is on */
static float   g_swipe_x, g_swipe_y;                    /* and where its finger is now */
static SDL_AtomicInt g_touch_btn_atomic, g_touch_stick_atomic;  /* for the game thread */
static SDL_AtomicInt g_remake;                       /* the surface changed: the render thread rebinds */
static SDL_AtomicInt g_background;                   /* the app is in the background: no drawing */

/* A group's place when it has been moved: its centre as fractions of the
 * screen, and its size against the default. */
typedef struct { int set; float cx, cy, k; } placed;
static placed g_place[G_COUNT];

/* sx, sy: where it went down, t0 when; dir: what the target button decided (TGT_*) */
typedef struct { SDL_FingerID id; int elem; float x, y, sx, sy; uint64_t t0; int dir; } finger;
enum { TGT_UNDECIDED, TGT_LOCK, TGT_LEFT, TGT_RIGHT };
#define FINGERS 10
static finger g_fingers[FINGERS];

static int vpad_visible(void) {
    if (g_edit) return 1;
    if (g_vpad_mode == VPAD_ON) return 1;
    if (g_vpad_mode == VPAD_OFF) return 0;
#ifdef __ANDROID__
    return !mhfu_gamepad_connected();
#else
    return 0;                                          /* the desktop has a keyboard */
#endif
}

/* The internal resolution the player chose in the menu (1-4 times 480x272;
 * 0: what the game was started with). The renderer takes it when it starts,
 * so a change shows after a restart. */
static int g_res_setting;
static int g_fps60;                  /* the 60 fps mode (mhfu_fps_set_preferred) */
static int g_wide;                   /* the widescreen mod (mhfu_mod_set_widescreen) */
/* A texture pack (texpack.c): 1 use one when there is one, 0 not; read at
 * start (texpack_off tells mhfu_texpack_init through MHFU_TEXPACK=0), so a
 * change shows after a restart. */
static int g_texpack = 1, g_texpack_started = -1;
/* The GE on a thread of its own (psprecomp PSPRECOMP_GE_THREAD): read at the start. */
static int g_gethread = 1, g_gethread_started = -1;
static char g_texstyle[256], g_texstyle_started[256];   /* its ADDONS style ("": MAIN's own) */
static int g_mod_set[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
                                     /* the built-in mods as settings.ini has them: -1 not there */

static char g_settings_path[1024];
static void settings_load(void) {
    char *pref = SDL_GetPrefPath("PateeerQ", "MHFU");
    if (!pref) return;
    snprintf(g_settings_path, sizeof g_settings_path, "%ssettings.ini", pref);
    SDL_free(pref);
    FILE *f = fopen(g_settings_path, "r");
    char line[160];
    while (f && fgets(line, sizeof line, f)) {
        int v;
        float x, y, k;
        char name[32];
        if (sscanf(line, "virtual_gamepad=%d", &v) == 1 && v >= 0 && v <= 2) g_vpad_mode = v;
        if (sscanf(line, "show_fps=%d", &v) == 1) g_show_fps_init = v;
        if (sscanf(line, "button_opacity=%d", &v) == 1 && v >= 10 && v <= 100) g_vpad_alpha = v;
        if (sscanf(line, "weapon_buttons=%d", &v) == 1) g_skills_on = v != 0;
        if (sscanf(line, "button_style=%d", &v) == 1 && v >= 0) g_style = v ? STYLE_MOBILE : STYLE_PSP;   /* 2: the old Auto */
        if (sscanf(line, "pad_combined=%d", &v) == 1) g_pad_combined = v != 0;
        if (sscanf(line, "resolution=%d", &v) == 1 && v >= 1 && v <= 4) g_res_setting = v;
        if (sscanf(line, "fps60=%d", &v) == 1) g_fps60 = v != 0;
        if (sscanf(line, "widescreen=%d", &v) == 1) g_wide = v != 0;
        if (sscanf(line, "texpack=%d", &v) == 1) g_texpack = v != 0;
        if (sscanf(line, "ge_thread=%d", &v) == 1) g_gethread = v != 0;
        if (!strncmp(line, "texpack_style=", 14)) {
            snprintf(g_texstyle, sizeof g_texstyle, "%s", line + 14);
            g_texstyle[strcspn(g_texstyle, "\r\n")] = 0;
        }
        char id[32];
        if (sscanf(line, "mod_%31[a-z0-9]=%d", id, &v) == 2)
            for (int i = 0; i < mhfu_mod_count(); i++)
                if (!strcmp(id, mhfu_mod_id(i))) g_mod_set[i] = v != 0;
        if (sscanf(line, "layout_%31[a-z]=%f,%f,%f", name, &x, &y, &k) == 4)
            for (int g = 0; g < G_COUNT; g++)
                if (!strcmp(name, k_group_name[g])) g_place[g] = (placed){ 1, x, y, k };
    }
    if (f) fclose(f);
    if (g_gethread_started < 0) {
        g_gethread_started = g_gethread;
        if (!getenv("PSPRECOMP_GE_THREAD")) {          /* main.c turns it on when nothing says */
#ifdef _WIN32
            _putenv_s("PSPRECOMP_GE_THREAD", g_gethread ? "1" : "0");
#else
            setenv("PSPRECOMP_GE_THREAD", g_gethread ? "1" : "0", 1);
#endif
        }
    }
    if (g_texpack_started < 0) {
        g_texpack_started = g_texpack;
        snprintf(g_texstyle_started, sizeof g_texstyle_started, "%s", g_texstyle);
#if defined(__ANDROID__)
        /* The desktop's launcher sets MHFU_TEXPACK itself. */
        if (!g_texpack && !getenv("MHFU_TEXPACK")) setenv("MHFU_TEXPACK", "0", 1);
        if (g_texstyle[0] && !getenv("MHFU_TEXPACK_STYLE")) setenv("MHFU_TEXPACK_STYLE", g_texstyle, 1);
#endif
    }
}
static void settings_save(void) {
    FILE *f = g_settings_path[0] ? fopen(g_settings_path, "w") : NULL;
    if (!f) return;
    fprintf(f, "# MHFU: the in-game menu's settings\nvirtual_gamepad=%d\nshow_fps=%d\nbutton_opacity=%d\n",
            g_vpad_mode, g_show_fps_init, g_vpad_alpha);
    if (g_res_setting) fprintf(f, "resolution=%d\n", g_res_setting);
    fprintf(f, "fps60=%d\nwidescreen=%d\nweapon_buttons=%d\nbutton_style=%d\ntexpack=%d\n", g_fps60, g_wide,
            g_skills_on, g_style, g_texpack);
    if (g_texstyle[0]) fprintf(f, "texpack_style=%s\n", g_texstyle);
    fprintf(f, "pad_combined=%d\nge_thread=%d\n", g_pad_combined, g_gethread);
    for (int i = 0; i < mhfu_mod_count(); i++) fprintf(f, "mod_%s=%d\n", mhfu_mod_id(i), mhfu_mod_on(i));
    for (int g = 0; g < G_COUNT; g++)
        if (g_place[g].set)
            fprintf(f, "layout_%s=%.4f,%.4f,%.3f\n", k_group_name[g], g_place[g].cx, g_place[g].cy, g_place[g].k);
    fclose(f);
}

/* Where each group is on a w x h screen: its centre and its size factor.
 * The shapes' own sizes follow from the screen's height times that factor. */
typedef struct { float w, h, x[G_COUNT], y[G_COUNT], k[G_COUNT]; } layout;
static layout layout_for(float w, float h) {
    layout L;
    L.w = w;
    L.h = h;
    const float def[G_COUNT][2] = {
        { 0.24f * h, h - 0.26f * h }, { 0.24f * h, 0.40f * h }, { w - 0.25f * h, 0.50f * h },
        { 0.20f * h, 0.09f * h }, { w - 0.20f * h, 0.09f * h },
        { w / 2 - 0.13f * h, h - 0.0725f * h }, { w / 2 + 0.13f * h, h - 0.0725f * h },
        { 0.08f * h, 0.20f * h },                      /* the menu button: top left, under the L pill (Start, chat right of it) */
        { w - 0.58f * h, h - 0.20f * h }, { w - 0.56f * h, 0.50f * h },
        { w - 0.26f * h, 0.60f * h }, { w - 0.13f * h, 0.88f * h }, { w - 0.44f * h, 0.88f * h },
        { w - 0.16f * h, 0.76f * h },
        { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },        /* draw, use, kick, bag: from the face buttons, below */
        { 0, 0 }, { 0, 0 }, { 0, 0 },                  /* yes/no, the menus' row, the action button: likewise */
        { 0, 0 },                                      /* pick up: the face buttons' middle */
    };
    for (int g = 0; g < G_COUNT; g++) {
        L.x[g] = g_place[g].set ? g_place[g].cx * w : def[g][0];
        L.y[g] = g_place[g].set ? g_place[g].cy * h : def[g][1];
        L.k[g] = g_place[g].set ? g_place[g].k : 1.0f;
    }
    /* The hunting buttons (Mobile, weapon sheathed) take the face buttons'
     * place: draw (Triangle) above, use (Square) left, kick (Select) right,
     * the bag (L) below. */
    const float fo = 0.12f * h * L.k[G_FACE];
    const float off[4][2] = { { 0, -1.1f }, { -1.2f, 0 }, { 1.2f, 0 }, { 0, 1.1f } };
    for (int i = 0; i < 4; i++) {
        const int g = G_DRAW + i;
        if (g_place[g].set) continue;
        L.x[g] = L.x[G_FACE] + off[i][0] * fo;
        L.y[g] = L.y[G_FACE] + off[i][1] * fo;
    }
    /* Yes and No on the face buttons' place, the menus' own row above them,
     * the action button (Talk, an object) above and left, clear of the
     * hunting buttons. */
    const float roff[3][2] = { { 0, 0 }, { 0, -2.35f }, { -1.6f, -1.3f } };
    for (int i = 0; i < 3; i++) {
        const int g = G_YESNO + i;
        if (g_place[g].set) continue;
        L.x[g] = L.x[G_FACE] + roff[i][0] * fo;
        L.y[g] = L.y[G_FACE] + roff[i][1] * fo;
    }
    if (!g_place[G_PICK].set) { L.x[G_PICK] = L.x[G_FACE]; L.y[G_PICK] = L.y[G_FACE]; }   /* amid draw/use/kick/bag */
    return L;
}

/* Each group's sizes, from the screen's height and its factor. */
static int pad_combined(void) { return g_pad_combined && g_style == STYLE_MOBILE; }
static float stick_r(const layout *L) { return (pad_combined() ? 0.21f : 0.15f) * L->h * L->k[G_STICK]; }
static float rstick_r(const layout *L) { return 0.13f * L->h * L->k[G_RSTICK]; }
static float dpad_r(const layout *L)  { return 0.15f * L->h * L->k[G_DPAD]; }
static float face_o(const layout *L)  { return 0.12f * L->h * L->k[G_FACE]; }
static float face_r(const layout *L)  { return 0.068f * L->h * L->k[G_FACE]; }
static float menu_r(const layout *L)  { return 0.055f * L->h * L->k[G_MENU]; }
static float skill_r(const layout *L) { return 0.062f * L->h * L->k[G_SKILL]; }
static float attack_r(const layout *L) { return 0.16f * L->h * L->k[G_ATTACK]; }
static float small_r(const layout *L, int g) { return (g == G_RUN ? 0.092f : 0.068f) * L->h * L->k[g]; }   /* Roll, Item, Run */

/* Where the hunter is: in a quest (game_task.ovl loaded, hle_mhfu.c) or not,
 * and with the weapon out: the hunter's weapon state at 0x090B3A52 is not 0
 * (0 sheathed, 1 out, 2 attacking, 3 Dual Blades' demon mode; from F9 dumps
 * and a trace on the phone). */
#define MHFU_WEAPON_OUT MHFU_ADDR_WEAPON_OUT
static int hunt_ctx(void) {
    if (!mhfu_in_quest() || !MHFU_WEAPON_OUT) return CTX_FREE;   /* 0: not known in this build */
    return psp_read8(MHFU_WEAPON_OUT) ? CTX_DRAWN : CTX_SHEATHED;
}
/* What the screen asks for (Mobile style), from the game's memory (region.h).
 * Yes is Cross and No is Circle in the western releases, the other way round
 * in the Japanese one and FUComplete. Talking is Cross in the western ones and
 * Circle in the Japanese one and FUComplete (act_bit); using a
 * door, the quest gate, the item box, a chair or the quest board is Square.
 * - UI_MENU: the title screens, the Start menu: Yes and No
 * - UI_TALK: an NPC in reach: a Talk button
 * - UI_OBJ: an object in reach: Square with the object's picture, and a
 *   second choice at some doors
 * - UI_WINDOW: in the lobby while a window, a question or a message is open:
 *   Yes, No and Square (a quest's details), always the same three (separate
 *   Next and detail buttons came and went, Muhammad) */
enum { UI_PLAY, UI_MENU, UI_TALK, UI_OBJ, UI_WINDOW };
#define WINDOW_ENTER_MS 120
#define WINDOW_KEEP_MS (MHFU_ADDR_LOBBY_WINDOW ? 250 : 1500)   /* JP/FUC's flag is exact */
/* The object in reach: its kind (region.h) and a door's destination. */
enum { OBJ_DOOR = 1, OBJ_GATE = 2, OBJ_TRAINING = 3, OBJ_BOX = 7, OBJ_CHAIR = 8, OBJ_BOARD = 9,
       OBJ_SUPPLY = 24, OBJ_BED = 30, OBJ_DELIVERY = 33,
       OBJ_SAVE_BED = 5 };                      /* the house's bed: save the game (Square) */   /* the last three: a quest's base camp (FUC dumps 19:17, 19:47) */
/* In a quest: only the Supply Box and the Delivery Box, opened with Circle and
 * only with the weapon sheathed. The lobby's prompt byte stays 0 there, but the
 * pointer goes back to 0 when the hunter walks off (11 dumps). */
static int quest_obj(int kind) { return kind == OBJ_SUPPLY || kind == OBJ_DELIVERY || kind == OBJ_BED; }
static int obj_now(int *kind, int *dest) {
    const int quest = mhfu_in_quest();
    if (!quest && !psp_read8(MHFU_ADDR_OBJ_PROMPT)) return 0;   /* no prompt shown: the pointer may be stale */
    if (quest && hunt_ctx() != CTX_SHEATHED) return 0;
    const uint32_t p = psp_read32(MHFU_ADDR_OBJ_TARGET);
    if (p < 0x08800000u || p > 0x0A000000u - 0x18u) return 0;
    *kind = psp_read8(p + 2) | psp_read8(p + 3) << 8;
    *dest = psp_read8(p + 0x16) | psp_read8(p + 0x17) << 8;
    return !quest || quest_obj(*kind);
}
/* Portable 2nd G and FUComplete (its English translation) keep the Japanese
 * confirm button: Circle is Yes, Cross is No. */
static int jp_buttons(void) { return !strcmp(MHFU_REGION_TAG, "JP") || !strcmp(MHFU_REGION_TAG, "FUC"); }
/* Talking to an NPC (village, Hall): Circle in JP/FUC, Cross in the western
 * releases (Muhammad on the phone, EU, 2026-11-10: Circle did not talk). In a
 * quest the action (Supply Box, carving, gathering) stays Circle everywhere:
 * Cross there crouches/dodges (phone, EU, the same day). */
static uint32_t act_bit(void) { return jp_buttons() ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS; }
/* The button an object takes: Circle for the quest's boxes, else Square. */
static uint32_t obj_bit(int kind) { return quest_obj(kind) ? PSP_CTRL_CIRCLE : PSP_CTRL_SQUARE; }
static int ui_ctx(void) {
    if (g_style != STYLE_MOBILE || g_edit) return UI_PLAY;
    const int lobby = mhfu_in_lobby(), quest = mhfu_in_quest();
    /* Once the screens after a quest begin (rewards, send to box), Yes/No stay
     * until game_task is gone: a reward movie or scene between them cleared the
     * flags and brought the hunting buttons back (Muhammad, 2026-11-10). */
    static int quest_done;
    if (!quest) quest_done = 0;
    else if (psp_read8(MHFU_ADDR_QUEST_PHASE) == 2) quest_done = 1;   /* not the prompt byte: a cancelled window would latch */
    if (quest_done) return UI_WINDOW;
    if (!lobby && !quest) return UI_MENU;                   /* title, character select, options */
    if (psp_read8(MHFU_ADDR_MENU_OPEN)) return UI_MENU;
    /* a window is open: the hunter is held (that bit also drops while running)
     * and a question, a message or a menu (prompt id 3 and up) is up */
    /* The window flags must hold WINDOW_ENTER_MS to count (one blinks for a
     * moment while running, when the hold bit is clear too: Yes/No showed up
     * mid-run, phone log 2026-11-09); once in, a gap up to WINDOW_KEEP_MS
     * (between a dialog's lines) keeps the buttons. */
    static int in_window;
    static uint64_t flags_since, flags_last;
    const uint64_t now = SDL_GetTicks();
    const unsigned hold = psp_read8(MHFU_ADDR_HUNTER_FREE);
    const int held = (lobby || quest) && !(hold & 0x04);
    /* In a quest a window or a screen (the Supply Box, Abandon Quest, "Quest
     * Retired", the save question) drops 0x20 as well as 0x04 (0xA4 -> 0x82 /
     * 0x80; FUC dumps 20:05, 20:58), but so do attacks, dodges and area changes
     * (Yes/No showed up there): the two window bytes must agree too. */
    int quest_held = 0;
    if (quest && !(hold & 0x24)) {
        const unsigned wa = psp_read8(MHFU_ADDR_QUEST_WIN_A), wb = psp_read8(MHFU_ADDR_QUEST_WIN_B);
        quest_held = (wa == 2 && wb == 5) || (wa == 10 && wb == 10);
    }
    /* The end-of-quest reward screen (Take Reward, Send to Item Box, End) keeps
     * the hunter "free" and those bytes 0, but sets the prompt byte (4; 0 in
     * every quest dump without a window): FUC dumps 22:47. 1 and 2 are an
     * object's prompt (2 seen standing still in a quest): windows are 3 up. */
    if (quest && psp_read8(MHFU_ADDR_UI_PROMPT) >= 3) quest_held = 1;
    if (quest && psp_read8(MHFU_ADDR_QUEST_PHASE) == 2) quest_held = 1;   /* the screens after a quest */
    /* The house's bed (save, "Exit Game?") leaves the lobby's window flag at 0:
     * the hunter's scripted-act byte tells it, with the hold bit down. */
    /* Only at that bed (object kind 5) with its prompt gone: the byte is 1 in
     * other acts too (walking through a door, standing at the bed), and Yes/No
     * blinked there (phone log 22:20). */
    int lobby_act = 0;
    if (lobby && held && psp_read8(MHFU_ADDR_HUNTER_ACT) == 1 && !psp_read8(MHFU_ADDR_OBJ_PROMPT)) {
        const uint32_t op = psp_read32(MHFU_ADDR_OBJ_TARGET);
        lobby_act = op >= 0x08800000u && op <= 0x0A000000u - 0x18u &&
                    (psp_read8(op + 2) | psp_read8(op + 3) << 8) == OBJ_SAVE_BED;
    }
    const int flags = lobby_act ||
                      (MHFU_ADDR_LOBBY_WINDOW && lobby
                           ? psp_read8(MHFU_ADDR_LOBBY_WINDOW) != 0                /* set from the first word */
                           : quest ? quest_held
                                   : held && (psp_read8(MHFU_ADDR_YESNO) || psp_read8(MHFU_ADDR_MESSAGE) ||
                                              psp_read8(MHFU_ADDR_UI_PROMPT) >= 3));
    if (flags) {
        if (!flags_since) flags_since = now;
        flags_last = now;
        if (now - flags_since >= WINDOW_ENTER_MS) in_window = 1;
    } else flags_since = 0;
    if (in_window && (quest ? quest_held : held || (MHFU_ADDR_LOBBY_WINDOW && lobby)) && now - flags_last < WINDOW_KEEP_MS)
        return UI_WINDOW;
    in_window = 0;
    /* Not in a quest: the pointer was set near a carcass in one set of dumps
     * (19:28) and stayed 0 at one in another (19:47); carving and gathering
     * have no flag found yet. */
    if (!quest && psp_read32(MHFU_ADDR_TALK_TARGET)) return UI_TALK;
    int kind, dest;
    if (obj_now(&kind, &dest)) return UI_OBJ;
    return UI_PLAY;
}
static uint32_t yes_bit(void) { return jp_buttons() ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS; }
static uint32_t no_bit(void) { return jp_buttons() ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE; }
static int g_uictx;                                     /* ui_ctx_settled(), once a frame */
static int g_obj_kind, g_obj_dest;                      /* with UI_OBJ: what is in reach */
/* ui_ctx() once it has held for UI_SETTLE_MS: the flags flicker for a frame or
 * two (walking past an NPC, a prompt redrawn), and the buttons blinked with them. */
#define UI_SETTLE_MS 150
static int g_ui_pending;                                /* a change is waiting to settle: wake soon */
static int ui_ctx_settled(void) {
    static int settled = UI_PLAY, pending = -1;
    static uint64_t since;
    const int now_ctx = ui_ctx();
    const uint64_t now = SDL_GetTicks();
    g_ui_pending = 0;
    if (now_ctx == settled) { pending = -1; return settled; }
    if (g_edit || g_style != STYLE_MOBILE) { pending = -1; return settled = now_ctx; }
    if (now_ctx != pending) { pending = now_ctx; since = now; }
    else if (now - since >= UI_SETTLE_MS) {
        settled = now_ctx;
        pending = -1;
        /* Which flags chose it, to find a wrong one from a phone's log. */
        static const char *const name[] = { "play", "menu", "talk", "object", "window" };
        printf("touch ui: %s (qwin %u/%u win %u free %02X yesno %u message %u prompt %u menu %u talk %08X obj %08X objprompt %u)\n",
               settled >= 0 && settled < 5 ? name[settled] : "?", psp_read8(MHFU_ADDR_QUEST_WIN_A),
               psp_read8(MHFU_ADDR_QUEST_WIN_B),
               MHFU_ADDR_LOBBY_WINDOW && mhfu_in_lobby() ? psp_read8(MHFU_ADDR_LOBBY_WINDOW) : 0u,
               psp_read8(MHFU_ADDR_HUNTER_FREE),
               psp_read8(MHFU_ADDR_YESNO), psp_read8(MHFU_ADDR_MESSAGE), psp_read8(MHFU_ADDR_UI_PROMPT),
               psp_read8(MHFU_ADDR_MENU_OPEN), psp_read32(MHFU_ADDR_TALK_TARGET), psp_read32(MHFU_ADDR_OBJ_TARGET),
               psp_read8(MHFU_ADDR_OBJ_PROMPT));
        fflush(stdout);
        return settled;
    }
    g_ui_pending = 1;
    return settled;
}
/* A door's second choice (0 none): the Gathering Hall's entrance also takes
 * Cross (Circle in the western releases), its way out Triangle (FUComplete dumps). */
static uint32_t obj_extra(int kind, int dest) {
    /* The prompt byte is 2 at an object with two choices (FUC dumps): the
     * Gathering Hall's entrance (destination 4) takes Cross in JP/FUC, the
     * other doors Triangle. The western releases swap Cross and Circle there as
     * they do for Yes/No: the Online Gathering Hall is Circle in the USA game
     * (Muhammad on the phone, 2026-11-10; EU assumed the same). */
    if (kind != OBJ_DOOR) return 0;
    if (psp_read8(MHFU_ADDR_UI_PROMPT) == 2 || dest == 5)
        return dest == 4 ? (jp_buttons() ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE) : PSP_CTRL_TRIANGLE;
    return 0;
}
/* In the village or the Gathering Hall, outside a quest (Mobile style): no face
 * buttons, no L (but in the Online Gathering Hall: the player board), and Run
 * (R) in place of the R pill while nothing else is asked; Talk and the
 * objects' choices come as buttons of their own. */
static int lobby_now(void) { return g_style == STYLE_MOBILE && !g_edit && mhfu_in_lobby() && !mhfu_in_quest(); }

/* The mobile buttons are on; the editor shows all of them, to place. */
static int mobile_now(void) { return g_style == STYLE_MOBILE && (g_edit || hunt_ctx() == CTX_DRAWN); }
static int g_l_held;                                   /* the bag (L toggle), below */
/* The layout editor's pages (Mobile style): each shows only the buttons that
 * are on screen together, so nothing is edited under something else. The
 * order: what a hunter needs first. Legacy PSP has the one page of PSP
 * buttons. */
enum { EP_SHEATHED, EP_COMBAT, EP_LOBBY, EP_YESNO, EP_MENUROW, EP_COUNT };
static const char *const k_edit_page[EP_COUNT] = {
    "Hunting: weapon sheathed", "Hunting: weapon out (combat)", "Village and Gathering Hall",
    "Yes / No (dialogs and menus)", "Menu buttons (sort, pages, zoom)" };
static int g_edit_page, g_edit_list;                   /* the page; its list is open */
static int edit_page_has(int g) {
    if (g == G_MENU) return 1;
    if (g_style != STYLE_MOBILE)
        return g == G_STICK || g == G_DPAD || g == G_FACE || g == G_L || g == G_R || g == G_SELECT || g == G_START ||
               g == G_SKILL;
    switch (g_edit_page) {
    case EP_SHEATHED: return g == G_STICK || g == G_DRAW || g == G_USE || g == G_KICK || g == G_BAG || g == G_PICK ||
                             g == G_ROLL;
    case EP_COMBAT: return g == G_STICK || g == G_ATTACK || g == G_ROLL || g == G_ITEM || g == G_SKILL;
    case EP_LOBBY: return g == G_STICK || g == G_ACT || g == G_L;
    case EP_YESNO: return g == G_STICK || g == G_YESNO;
    case EP_MENUROW: return g == G_YESNO || g == G_EXTRAS;
    }
    return 0;
}
static int group_shown(int g) {
    if (g == G_RSTICK) return 0;                       /* the camera is a swipe now */
    if (g == G_DPAD && pad_combined()) return 0;       /* it is the stick's ring */
    const int hunt_new = g == G_DRAW || g == G_USE || g == G_KICK || g == G_BAG || g == G_PICK;
    const int ctx_group = g == G_YESNO || g == G_EXTRAS || g == G_ACT;
    if (ctx_group && !g_edit) return 0;               /* placed in the editor; drawn by the context */
    if (hunt_new && g_style != STYLE_MOBILE) return 0;   /* the Mobile style's own */
    if (g_edit) return edit_page_has(g) && g != G_RUN;
    if ((g_uictx == UI_MENU || g_uictx == UI_WINDOW) &&
        (g == G_FACE || g == G_L || g == G_R || g == G_SKILL || g == G_ATTACK || g == G_ROLL || g == G_ITEM || hunt_new))
        return 0;                                      /* Yes and No instead */
    if (lobby_now()) {
        if (g == G_FACE || g == G_R || g == G_SELECT || g == G_START || hunt_new) return 0;   /* Select: nothing there; Start: by the menu */
        if (g == G_L) return g_uictx == UI_PLAY && mhfu_adhoc_in_group();
    }
    if (g_style == STYLE_MOBILE && mhfu_in_quest()) {
        /* Hunting: no L, R, Select or face buttons; Start sits by the menu
         * button. Sheathed: draw, use, kick, the bag and Cross (crouch, or a
         * dodge while running); drawn: the attack pad and its buttons. */
        if (g == G_L || g == G_R || g == G_SELECT || g == G_START || g == G_FACE) return 0;
        const int sheathed = hunt_ctx() == CTX_SHEATHED;
        /* the bag open: only it and its sides (Square, Circle) */
        if (hunt_new) return sheathed && (g == G_BAG || !g_l_held);
        if (g == G_ROLL && sheathed) return !g_l_held;
    } else if (hunt_new) return 0;
    const int m = mobile_now();
    if (g == G_FACE || g == G_DPAD) return !m;
    if (!m) return g != G_ATTACK && g != G_ROLL && g != G_ITEM && g != G_RUN;
    if (g == G_R) return 0;                            /* Run, the pad and the weapon buttons have it */
    if (g == G_RUN) return 0;                          /* running is the stick at its edge (lobby) */
    return 1;
}

/* The attack pad, from the knob (-1..1, y down): held near the middle it is
 * Triangle; pushed up, Triangle + Circle; sideways, Circle; down, R (each
 * weapon's own: guard, spirit, demon mode, charge, aim). */
static uint32_t attack_bits(float dx, float dy) {
    if (dx * dx + dy * dy < 0.40f * 0.40f) return PSP_CTRL_TRIANGLE;
    if (fabsf(dy) >= fabsf(dx)) return dy < 0 ? PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE : PSP_CTRL_RTRIGGER;
    return PSP_CTRL_CIRCLE;
}

/* The pictures on the buttons (icon(), below the layout): no words on them. */
enum { ICON_CROUCH, ICON_DODGE, ICON_RUN, ICON_SHEATHE, ICON_ITEM, ICON_SWORD, ICON_GUARD, ICON_AIM, ICON_TARGET,
       ICON_DEMON, ICON_SPIRIT, ICON_DANCE, ICON_FADE, ICON_CHARGE, ICON_TALK,
       ICON_CHEST, ICON_DOOR, ICON_CHAIR, ICON_BOARD, ICON_QUEST, ICON_TRAIN, ICON_DELIVERY, ICON_CARVE, ICON_SLEEP, ICON_BAG, ICON_KICK, ICON_CHAT, ICON_DRAW, ICON_USE, ICON_SORT, ICON_DELETE, ICON_ZOOM, ICON_NEEDS, ICON_SAVE, ICON_PICK, ICON_VIEW };

/* The weapon buttons: a column left of the face buttons whose buttons follow
 * the weapon the hunter carries, each holding a PSP combination (the game
 * sees the buttons pressed together). The class is the game's own id at
 * 0x090B3628 (CLAUDE.md), the same in the village and in a quest; they show
 * only in a quest (before one, the class is whatever the save holds, even on
 * the loading screens). With the Target Camera mod on, every weapon also gets
 * its target button (L + Up). */
#define MHFU_WEAPON_CLASS MHFU_ADDR_WEAPON_CLASS
#define SKILLS 3
typedef struct { int pic; uint32_t bits; } skill;
#define GUARD { ICON_GUARD, PSP_CTRL_RTRIGGER }
#define AIM   { ICON_AIM, PSP_CTRL_RTRIGGER }
static const skill k_skills[11][SKILLS] = {
    [0] = { GUARD },                                   /* Great Sword */
    [1] = { AIM },                                     /* Heavy Bowgun */
    [2] = { { ICON_CHARGE, PSP_CTRL_RTRIGGER } },      /* Hammer */
    [3] = { GUARD },                                   /* Lance */
    [4] = { GUARD },                                   /* Sword and Shield */
    [5] = { AIM },                                     /* Light Bowgun */
    [6] = { { ICON_DEMON, PSP_CTRL_RTRIGGER }, { ICON_DANCE, PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE } },  /* Dual Blades */
    [7] = { { ICON_SPIRIT, PSP_CTRL_RTRIGGER }, { ICON_FADE, PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE } },  /* Long Sword */
    [9] = { GUARD },                                   /* Gunlance */
    [10] = { AIM },                                    /* Bow */
};
#undef GUARD
#undef AIM
static int skills_now(skill out[SKILLS]) {
    int n = 0;
    if (!g_skills_on || hunt_ctx() != CTX_DRAWN) return 0;   /* only with the weapon out */
    const unsigned cls = psp_read8(MHFU_WEAPON_CLASS);
    if (cls < 11)
        for (int i = 0; i < SKILLS && k_skills[cls][i].bits; i++) out[n++] = k_skills[cls][i];
    for (int i = 0; i < mhfu_mod_count() && n < SKILLS; i++)
        if (!strcmp(mhfu_mod_id(i), "targetcam") && mhfu_mod_on(i))
            out[n++] = (skill){ ICON_TARGET, PSP_CTRL_LTRIGGER | PSP_CTRL_DOWN };   /* see target_bits */
    return n;
}
/* Where button i of n is: the column is centred on the group. */
static void skill_pos(const layout *L, int i, int n, float *x, float *y) {
    *x = L->x[G_SKILL];
    *y = L->y[G_SKILL] + (i - (n - 1) / 2.0f) * skill_r(L) * 2.4f;
}
/* L, R, Select and Start are pills: half their width and height. */
static void pill_size(const layout *L, int g, float *hw, float *hh) {
    const int shoulder = g == G_L || g == G_R;
    *hw = (shoulder ? 0.15f : 0.10f) * L->h * L->k[g];
    *hh = (shoulder ? 0.05f : 0.0375f) * L->h * L->k[g];
}

/* A group's bounds, for the editor. */
static void yes_pos(const layout *L, float *x, float *y);
static void no_pos(const layout *L, float *x, float *y);
static float big_r(const layout *L);
static float yn_o(const layout *L);
static float yn_r(const layout *L);
static float wx_r(const layout *L);
static float act_r(const layout *L);
static void group_box(const layout *L, int g, float *x0, float *y0, float *x1, float *y1) {
    float rx, ry;
    switch (g) {
    case G_STICK: rx = ry = stick_r(L); break;
    case G_RSTICK: rx = ry = rstick_r(L); break;
    case G_DPAD: rx = ry = dpad_r(L); break;
    case G_FACE: rx = ry = face_o(L) + face_r(L); break;
    case G_MENU: rx = ry = menu_r(L); break;
    case G_SKILL: rx = skill_r(L); ry = skill_r(L) * 3.4f; break;      /* room for all three */
    case G_ATTACK: rx = ry = attack_r(L); break;
    case G_ROLL: case G_ITEM: case G_RUN: rx = ry = small_r(L, g); break;
    case G_DRAW: case G_USE: case G_KICK: case G_BAG: case G_PICK: rx = ry = 0.06f * L->h * L->k[g]; break;
    case G_YESNO: {
        float ax, ay, bx2, by2;
        no_pos(L, &ax, &ay);
        yes_pos(L, &bx2, &by2);
        rx = fmaxf(L->x[g] - ax, bx2 - L->x[g]) + big_r(L);
        ry = yn_o(L) * 1.15f + yn_r(L);
        break;
    }
    case G_EXTRAS: rx = 0.12f * L->h * L->k[g] * 1.3f * 2 + wx_r(L); ry = wx_r(L); break;
    case G_ACT: rx = act_r(L) * 3.4f; ry = act_r(L); break;
    default: pill_size(L, g, &rx, &ry); break;
    }
    *x0 = L->x[g] - rx; *y0 = L->y[g] - ry; *x1 = L->x[g] + rx; *y1 = L->y[g] + ry;
}

static float dist(float ax, float ay, float bx, float by) { return sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }

/* Yes and No sit where the face buttons are, No left of Yes; the action button (Talk, an object) a little
 * above the face buttons' middle in the lobby (where they are not), above and
 * left of them in a quest; a door's second choice left of it. */
static float yn_o(const layout *L) { return 0.12f * L->h * L->k[G_YESNO]; }
static float yn_r(const layout *L) { return 0.068f * L->h * L->k[G_YESNO]; }
static float big_r(const layout *L) { return yn_r(L) * 1.35f; }
static float act_r(const layout *L) { return 0.068f * L->h * L->k[G_ACT] * 1.35f; }
static float wx_r(const layout *L) { return 0.068f * L->h * L->k[G_EXTRAS]; }
static void yes_pos(const layout *L, float *x, float *y) { *x = L->x[G_YESNO] + yn_o(L) * 0.75f; *y = L->y[G_YESNO] + yn_o(L) * 0.55f; }
static void no_pos(const layout *L, float *x, float *y) { *x = L->x[G_YESNO] - yn_o(L) * 1.35f; *y = L->y[G_YESNO] + yn_o(L) * 0.55f; }
static void act_pos(const layout *L, float *x, float *y) { *x = L->x[G_ACT]; *y = L->y[G_ACT]; }
static void act2_pos(const layout *L, float *x, float *y) { act_pos(L, x, y); *x -= act_r(L) * 2.3f; }
/* The object's picture. */
static int obj_icon(int kind) {
    switch (kind) {
    case OBJ_GATE: return ICON_QUEST;
    case OBJ_TRAINING: return ICON_TRAIN;
    case OBJ_BOX: return ICON_CHEST;
    case OBJ_CHAIR: return ICON_CHAIR;
    case OBJ_BOARD: return ICON_BOARD;
    case OBJ_SUPPLY: return ICON_CHEST;
    case OBJ_DELIVERY: return ICON_DELIVERY;
    case OBJ_BED: return ICON_SLEEP;
    case OBJ_SAVE_BED: return ICON_SAVE;
    default: return -1;                                /* doors: the plain Square mark */
    }
}
static int asks_yes_no(void) { return g_uictx == UI_MENU || g_uictx == UI_WINDOW; }
/* Square (a quest's details) above Yes and No, in the lobby's windows. */
static void detail_pos(const layout *L, float *x, float *y) { *x = L->x[G_YESNO] - yn_o(L) * 0.30f; *y = L->y[G_YESNO] - yn_o(L) * 1.15f; }
/* Not at a bookshelf (object kind 10): its question takes Yes and No only. */
static int detail_shown(void) {
    if (g_uictx != UI_WINDOW) return 0;
    const uint32_t p = psp_read32(MHFU_ADDR_OBJ_TARGET);
    if (p < 0x08800000u || p > 0x0A000000u - 0x18u) return 1;
    const int kind = psp_read8(p + 2) | psp_read8(p + 3) << 8;
    return kind != 10 && kind != OBJ_SAVE_BED && kind != OBJ_SUPPLY && kind != OBJ_DELIVERY;   /* bookshelf, bed, boxes: Yes and No */
}

/* Hunting (Mobile): the bag holds L as a toggle (the item bar), with Square
 * (left) and Circle (right) beside it while held; draw, use and kick are
 * Triangle, Square and Select; Start sits left of the menu button. */
static int quest_buttons(void) { return g_style == STYLE_MOBILE && !g_edit && mhfu_in_quest(); }
/* Start as a round button under the menu button: hunting, the village and the halls (Mobile). */
static int start_by_menu(void) { return quest_buttons() || lobby_now(); }
static float hunt_r(const layout *L, int g) { return 0.06f * L->h * L->k[g]; }
static int g_l_held;
static int l_toggles(void);
static int l_side_shown(void) { return g_l_held && l_toggles() && group_shown(G_BAG); }
static float l_side_r(const layout *L) { return face_r(L) * 0.85f; }
static void l_side_pos(const layout *L, int right, float *x, float *y) {
    *x = L->x[G_BAG] + (right ? 1.0f : -1.0f) * (hunt_r(L, G_BAG) + l_side_r(L) * 1.25f);
    *y = L->y[G_BAG];
}
static void qstart_pos(const layout *L, float *x, float *y) { *x = L->x[G_MENU]; *y = L->y[G_MENU] + menu_r(L) * 2.6f; }

/* The item box's sub-menus (JP/FUC, lobby_task's memory; FUC dumps 20:25):
 * MHFU_ADDR_BOX_SUB is 1 in a sub-menu, 0 on the box's own menu, and
 * MHFU_ADDR_BOX_KIND tells which: 0 put in (sort, quantity = Triangle), 1 take
 * out / combine and the like (L, sort, R), 2 change equipment (page back =
 * Triangle + L, page on = Triangle + R, sort), 4 the equipment sets (delete =
 * Triangle). Info is the Square already there. (The prompt byte tried first
 * follows the highlighted line's help text: the buttons blinked.) */
enum { WX_SORT, WX_PAGE_PREV, WX_PAGE_NEXT, WX_LEFT, WX_RIGHT, WX_DELETE, WX_QTY, WX_ZOOM, WX_NEEDS, WX_VIEW };
static int g_win_id;                                     /* 1 + the sub-menu's kind, 0 none */
/* Which of the box's screens is up (FUC dumps 20:25, 21:04, 21:11): only at
 * the item box (object kind 6 or 7; a bookshelf's Yes/No got the box's
 * buttons), in a sub-menu (BOX_SUB), by the box menu's line (BOX_MAIN: 0 put
 * in, 1 take out, 2 combine, 3 change equipment, 4 arrange, 5 sell) and, under
 * change equipment, BOX_KIND (2 the equipment list, 4 editing a set; 1 its own
 * four-line menu). Returns 1 put in, 2 take out / combine, 3 the equipment
 * list, 4 editing a set, 0 nothing extra. */
static int box_menu(void) {
    if (MHFU_ADDR_CRAFT_SCREEN && mhfu_in_lobby()) {     /* the smithy: crafting 6, decorations 7 */
        const int cs = psp_read8(MHFU_ADDR_CRAFT_SCREEN);
        if (cs == 2) return 6;
        if (cs == 5) return 7;
    }
    if (!MHFU_ADDR_BOX_SUB || !mhfu_in_lobby() || !psp_read8(MHFU_ADDR_BOX_SUB)) return 0;
    const uint32_t p = psp_read32(MHFU_ADDR_OBJ_TARGET);
    if (p < 0x08800000u || p > 0x0A000000u - 0x18u) return 0;
    const int obj = psp_read8(p + 2) | psp_read8(p + 3) << 8;
    if (obj != 6 && obj != OBJ_BOX) return 0;
    const int kind = psp_read8(MHFU_ADDR_BOX_KIND), line = psp_read8(MHFU_ADDR_BOX_MAIN);
    /* Change equipment, arrange and sell open a choice first (four lines, two
     * lines) and their list under it: BOX_DEEP is 1 only once the list is up
     * (the choice showed the list's buttons before). */
    if (line >= 3 && !psp_read8(MHFU_ADDR_BOX_DEEP)) return 0;
    switch (line) {
    case 0: return 1;                                    /* put in */
    case 1: case 2: case 4: case 5: return 2;            /* take out, combine, arrange, sell: lists */
    case 3: return kind == 2 ? 3 : kind == 4 ? 4 : 0;    /* the equipment list, editing a set */
    default: return 0;
    }
}
static int win_extras(int out[6]) {
    if (g_uictx != UI_WINDOW) return 0;
    switch (g_win_id) {
    case 1: out[0] = WX_SORT; out[1] = WX_QTY; return 2;
    case 2: out[0] = WX_LEFT; out[1] = WX_SORT; out[2] = WX_RIGHT; return 3;
    case 3: out[0] = WX_PAGE_PREV; out[1] = WX_LEFT; out[2] = WX_SORT; out[3] = WX_RIGHT; out[4] = WX_PAGE_NEXT; return 5;
    case 4: out[0] = WX_DELETE; return 1;
    case 6:                                                         /* crafting: zoom (L), requirements (R) */
        out[0] = WX_ZOOM; out[1] = WX_NEEDS;
        /* EU/US show the weapon only on View (Select); FUComplete shows it by itself
         * (Muhammad, 2026-11-10; JP not known: it keeps FUC's two) */
        if (!jp_buttons()) { out[2] = WX_VIEW; return 3; }
        return 2;
    case 7: out[0] = WX_LEFT; out[1] = WX_RIGHT; return 2;          /* decorations: pages (L, R) */
    default: return 0;
    }
}
static uint32_t wx_bits(int k) {
    switch (k) {
    case WX_SORT: return PSP_CTRL_SELECT;
    case WX_PAGE_PREV: return PSP_CTRL_TRIANGLE | PSP_CTRL_LTRIGGER;
    case WX_PAGE_NEXT: return PSP_CTRL_TRIANGLE | PSP_CTRL_RTRIGGER;
    case WX_LEFT: return PSP_CTRL_LTRIGGER;
    case WX_RIGHT: return PSP_CTRL_RTRIGGER;
    case WX_DELETE: return PSP_CTRL_TRIANGLE;
    case WX_QTY: return PSP_CTRL_TRIANGLE;
    case WX_ZOOM: return PSP_CTRL_LTRIGGER;
    case WX_NEEDS: return PSP_CTRL_RTRIGGER;
    case WX_VIEW: return PSP_CTRL_SELECT;
    }
    return 0;
}
static void wx_pos(const layout *L, int i, int n, float *x, float *y) {
    /* Centred on the face buttons, but a long row (the equipment list's five)
     * ends at most 1.3 steps right of them: it grows to the left. */
    const float step = 0.12f * L->h * L->k[G_EXTRAS] * 1.3f, half = (n - 1) / 2.0f * step;
    *x = L->x[G_EXTRAS] + (i - (n - 1) / 2.0f) * step - (half > step ? half - step : 0.0f);
    *y = L->y[G_EXTRAS];
}

/* The control under (x, y), in window pixels. Generous: a thumb is wide. */
static int hit(const layout *L, float x, float y) {
    if (dist(x, y, L->x[G_MENU], L->y[G_MENU]) < menu_r(L) * 1.4f) return T_MENU;
    if (mhfu_chat_available() && dist(x, y, L->x[G_MENU], L->y[G_MENU] + menu_r(L) * (start_by_menu() ? 5.2f : 2.6f)) < menu_r(L) * 1.3f)
        return T_CHAT;
    float bx, by;
    if (asks_yes_no()) {
        yes_pos(L, &bx, &by);
        if (dist(x, y, bx, by) < big_r(L) * 1.25f) return T_YES;
        no_pos(L, &bx, &by);
        if (dist(x, y, bx, by) < big_r(L) * 1.25f) return T_NO;
        if (detail_shown()) {
            detail_pos(L, &bx, &by);
            if (dist(x, y, bx, by) < yn_r(L) * 1.4f) return T_DETAIL;
        }
    } else if (g_uictx == UI_TALK || g_uictx == UI_OBJ) {
        act_pos(L, &bx, &by);
        if (dist(x, y, bx, by) < act_r(L) * 1.2f) return T_ACT;
        if (g_uictx == UI_OBJ && obj_extra(g_obj_kind, g_obj_dest)) {
            act2_pos(L, &bx, &by);
            if (dist(x, y, bx, by) < act_r(L) * 1.2f) return T_ACT2;
        }
    }
    if (l_side_shown()) {
        l_side_pos(L, 0, &bx, &by);
        if (dist(x, y, bx, by) < l_side_r(L) * 1.3f) return T_LLEFT;
        l_side_pos(L, 1, &bx, &by);
        if (dist(x, y, bx, by) < l_side_r(L) * 1.3f) return T_LRIGHT;
    }
    if (start_by_menu()) {
        qstart_pos(L, &bx, &by);
        if (dist(x, y, bx, by) < menu_r(L) * 1.3f) return T_QSTART;
    }
    if (quest_buttons()) {
        static const int hg[5] = { G_DRAW, G_USE, G_KICK, G_BAG, G_PICK }, he[5] = { T_DRAW, T_USE, T_KICK, T_BAG, T_PICK };
        for (int i = 0; i < 5; i++)
            if (group_shown(hg[i]) && dist(x, y, L->x[hg[i]], L->y[hg[i]]) < hunt_r(L, hg[i]) * 1.3f) return he[i];
    }
    {
        int wx[6];
        const int nw = win_extras(wx);
        for (int i = 0; i < nw; i++) {
            wx_pos(L, i, nw, &bx, &by);
            if (dist(x, y, bx, by) < wx_r(L) * 1.25f) return T_WX0 + i;
        }
    }
    static const int pills[4] = { G_L, G_R, G_SELECT, G_START }, pill_elem[4] = { T_L, T_R, T_SELECT, T_START };
    for (int i = 0; i < 4; i++) {
        float hw, hh;
        if (!group_shown(pills[i])) continue;
        pill_size(L, pills[i], &hw, &hh);
        if (fabsf(x - L->x[pills[i]]) < hw * 1.1f && fabsf(y - L->y[pills[i]]) < hh * 1.6f) return pill_elem[i];
    }
    const int m = mobile_now() && !asks_yes_no();
    if (m) {
        if (dist(x, y, L->x[G_ROLL], L->y[G_ROLL]) < small_r(L, G_ROLL) * 1.3f) return T_ROLL;
        if (dist(x, y, L->x[G_ITEM], L->y[G_ITEM]) < small_r(L, G_ITEM) * 1.3f) return T_ITEM;
        if (group_shown(G_RUN) && dist(x, y, L->x[G_RUN], L->y[G_RUN]) < small_r(L, G_RUN) * 1.3f) return T_RUN;
        if (dist(x, y, L->x[G_ATTACK], L->y[G_ATTACK]) < attack_r(L) * 1.15f) return T_ATTACK;
    } else if (group_shown(G_ROLL) && dist(x, y, L->x[G_ROLL], L->y[G_ROLL]) < small_r(L, G_ROLL) * 1.3f) {
        return T_ROLL;
    } else if (group_shown(G_RUN) && dist(x, y, L->x[G_RUN], L->y[G_RUN]) < small_r(L, G_RUN) * 1.3f) {
        return T_RUN;
    } else if (group_shown(G_FACE)) {
        const float fo = face_o(L);
        const struct { float ox, oy; int e; } face[4] = {
            { 0, fo, T_CROSS }, { fo, 0, T_CIRCLE }, { -fo, 0, T_SQUARE }, { 0, -fo, T_TRIANGLE } };
        int best = T_NONE;
        float bd = face_r(L) * 1.5f;
        for (int i = 0; i < 4; i++) {
            const float d = dist(x, y, L->x[G_FACE] + face[i].ox, L->y[G_FACE] + face[i].oy);
            if (d < bd) { bd = d; best = face[i].e; }
        }
        if (best) return best;
    }
    skill sk[SKILLS];
    const int ns = group_shown(G_SKILL) ? skills_now(sk) : 0;
    for (int i = 0; i < ns; i++) {
        float sx, sy;
        skill_pos(L, i, ns, &sx, &sy);
        if (dist(x, y, sx, sy) < skill_r(L) * 1.2f) return T_SKILL0 + i;
    }
    if (pad_combined()) {
        /* one round control: the middle is the stick, the ring the d-pad */
        const float d = dist(x, y, L->x[G_STICK], L->y[G_STICK]), r = stick_r(L);
        if (d < r * PAD_INNER) return T_STICK;
        if (d < r * 1.25f) return T_DPAD;
        return T_NONE;
    }
    if (!m && dist(x, y, L->x[G_DPAD], L->y[G_DPAD]) < dpad_r(L) * 1.25f) return T_DPAD;
    if (dist(x, y, L->x[G_STICK], L->y[G_STICK]) < stick_r(L) * 1.6f) return T_STICK;
    return T_NONE;
}

static uint32_t elem_button(int e) {
    switch (e) {
    case T_CROSS: return PSP_CTRL_CROSS;     case T_CIRCLE: return PSP_CTRL_CIRCLE;
    case T_SQUARE: return PSP_CTRL_SQUARE;   case T_TRIANGLE: return PSP_CTRL_TRIANGLE;
    case T_L: return PSP_CTRL_LTRIGGER;      case T_R: return PSP_CTRL_RTRIGGER;
    case T_SELECT: return PSP_CTRL_SELECT;   case T_START: return PSP_CTRL_START;
    case T_ROLL: return PSP_CTRL_CROSS;      case T_ITEM: return PSP_CTRL_SQUARE;
    case T_RUN: return PSP_CTRL_RTRIGGER;
    case T_YES: return yes_bit();
    case T_NO: return no_bit();
    case T_ACT: return g_uictx == UI_TALK ? act_bit() : g_uictx == UI_OBJ ? obj_bit(g_obj_kind) : 0;
    case T_ACT2: return g_uictx == UI_OBJ ? obj_extra(g_obj_kind, g_obj_dest) : 0;
    case T_DETAIL: return detail_shown() ? PSP_CTRL_SQUARE : 0;
    case T_LLEFT: return l_side_shown() ? PSP_CTRL_SQUARE : 0;
    case T_LRIGHT: return l_side_shown() ? PSP_CTRL_CIRCLE : 0;
    case T_DRAW: return PSP_CTRL_TRIANGLE;
    case T_USE: return PSP_CTRL_SQUARE;
    case T_KICK: return PSP_CTRL_SELECT;
    case T_PICK: return PSP_CTRL_CIRCLE;   /* gather, carve, pick up (Circle in every release) */
    case T_QSTART: return PSP_CTRL_START;
    case T_WX0: case T_WX1: case T_WX2: case T_WX3: case T_WX4: case T_WX5: {
        int wx[6];
        const int nw = win_extras(wx);
        return e - T_WX0 < nw ? wx_bits(wx[e - T_WX0]) : 0;
    }
    case T_SKILL0: case T_SKILL1: case T_SKILL2: {
        skill sk[SKILLS];
        return e - T_SKILL0 < skills_now(sk) ? sk[e - T_SKILL0].bits : 0;
    }
    default: return 0;
    }
}

/* The target button (Target Camera mod): a tap, or a finger held still, is
 * L + Down (lock on); slid left or right it is L + Left / L + Right (the
 * next target that way). Nothing is sent until it is told which, so a slide
 * never locks first: held still TARGET_HOLD_MS, or slid past a twentyfifth
 * of the screen's height. A quick tap sends L + Down for TARGET_TAP_MS once
 * the finger is up. Muhammad's layout, 2026-11-06. */
#define TARGET_HOLD_MS 150
#define TARGET_TAP_MS  120
static uint64_t g_target_tap_until;
static int is_target_elem(int e) {
    if (e < T_SKILL0 || e > T_SKILL2) return 0;
    skill sk[SKILLS];
    return e - T_SKILL0 < skills_now(sk) && sk[e - T_SKILL0].pic == ICON_TARGET;
}
static uint32_t target_bits(finger *f, float h) {
    const float dx = f->x - f->sx;
    if (dx < -h * 0.04f) f->dir = TGT_LEFT;
    else if (dx > h * 0.04f) f->dir = TGT_RIGHT;
    else if (f->dir == TGT_UNDECIDED && SDL_GetTicks() - f->t0 >= TARGET_HOLD_MS) f->dir = TGT_LOCK;
    switch (f->dir) {
    case TGT_LOCK:  return PSP_CTRL_LTRIGGER | PSP_CTRL_DOWN;
    case TGT_LEFT:  return PSP_CTRL_LTRIGGER | PSP_CTRL_LEFT;
    case TGT_RIGHT: return PSP_CTRL_LTRIGGER | PSP_CTRL_RIGHT;
    default:        return 0;
    }
}
/* touch_update needs to run again by itself (time decides, not a finger). */
static int target_pending(void) {
    if (g_target_tap_until && SDL_GetTicks() < g_target_tap_until + 50) return 1;
    for (int i = 0; i < FINGERS; i++)
        if (g_fingers[i].elem && is_target_elem(g_fingers[i].elem) && g_fingers[i].dir == TGT_UNDECIDED) return 1;
    return 0;
}

/* The PSP state of every finger, into what the game reads. */
#define STICK_RUN 0.95f                        /* how far out the stick runs, of its radius */
/* In a quest (Mobile) L is a toggle: a tap holds it (the item bar, L + Square
 * to use, L + Circle / Square to pick), another lets go (Muhammad, 2026-11-09). */
static int l_toggles(void) { return quest_buttons() && hunt_ctx() == CTX_SHEATHED; }
/* Where the stick at its edge runs: the lobby, and a quest with the weapon
 * sheathed (drawn, R is the weapon's own: guard, aim, ...). Mobile only. */
static int stick_runs(void) {
    if (g_style != STYLE_MOBILE || g_edit) return 0;
    /* Not while a menu or a window is up: there R is the menu's own (the
     * smithy's requirements, pages), and the stick only moves the cursor. */
    if (g_uictx == UI_MENU || g_uictx == UI_WINDOW) return 0;
    return lobby_now() || (mhfu_in_quest() && hunt_ctx() == CTX_SHEATHED);
}
static int g_stick_run;                        /* it does now: its outline glows */
static uint64_t g_held_elems;                   /* the touch elements a finger is on (T_*) */
static int elem_held(int e) { return e > 0 && e < 64 && (g_held_elems >> e & 1); }
static void touch_update(float w, float h) {
    const layout L = layout_for(w, h);
    uint32_t b = 0;
    int ax = -1, ay = -1, rheld = 0, sd = 0, atk = 0, run = 0;
    float rx = 0, ry = 0;
    for (int i = 0; i < FINGERS; i++) {
        const finger *f = &g_fingers[i];
        if (!f->elem) continue;
        if (f->elem == T_STICK) {
            const float r = stick_r(&L) * (pad_combined() ? PAD_INNER * 0.85f : 1.0f);
            float dx = (f->x - L.x[G_STICK]) / r, dy = (f->y - L.y[G_STICK]) / r;
            const float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            ax = (int)(128 + dx * 127);
            ay = (int)(128 + dy * 127);
            /* In the lobby the stick pushed to its edge runs (holds R): no
             * Run button there (Muhammad, 2026-11-09). */
            if (m >= STICK_RUN && stick_runs()) { b |= PSP_CTRL_RTRIGGER; run = 1; }
        } else if (f->elem == T_ATTACK) {
            const float r = attack_r(&L);
            float dx = (f->x - L.x[G_ATTACK]) / r, dy = (f->y - L.y[G_ATTACK]) / r;
            const float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            b |= attack_bits(dx, dy);
            g_attack_kx = dx;
            g_attack_ky = dy;
            atk = 1;
        } else if (f->elem == T_SWIPE) {
            /* The camera: a finger on the screen away from the buttons, dragged
             * from where it went down, holds the d-pad that way (MHFU turns
             * the camera with it): sideways turns, up and down (a longer drag,
             * so a sideways one does not tilt too) look up and down. */
            const float dx = f->x - f->sx, dy = f->y - f->sy;
            if (dx > L.h * 0.04f) b |= PSP_CTRL_RIGHT;
            if (dx < -L.h * 0.04f) b |= PSP_CTRL_LEFT;
            if (dy > L.h * 0.08f) b |= PSP_CTRL_DOWN;
            if (dy < -L.h * 0.08f) b |= PSP_CTRL_UP;
            rx = f->sx;
            ry = f->sy;
            rheld = 1;
            g_swipe_x = f->x;
            g_swipe_y = f->y;
        } else if (f->elem == T_DPAD) {
            const int gd = pad_combined() ? G_STICK : G_DPAD;
            const float dx = f->x - L.x[gd], dy = f->y - L.y[gd];
            if (sqrtf(dx * dx + dy * dy) > (pad_combined() ? stick_r(&L) * 0.3f : dpad_r(&L) * 0.2f)) {
                const float a = atan2f(dy, dx) * 180 / 3.14159265f;     /* 0 = right, 90 = down */
                if (a > -67.5f && a < 67.5f) b |= PSP_CTRL_RIGHT;
                if (a > 22.5f && a < 157.5f) b |= PSP_CTRL_DOWN;
                if (a > 112.5f || a < -112.5f) b |= PSP_CTRL_LEFT;
                if (a > -157.5f && a < -22.5f) b |= PSP_CTRL_UP;
            }
        } else if (is_target_elem(f->elem)) {
            b |= target_bits(&g_fingers[i], h);
            sd |= 1 << (f->elem - T_SKILL0);
        } else {
            b |= elem_button(f->elem);
            if (f->elem >= T_SKILL0 && f->elem <= T_SKILL2) sd |= 1 << (f->elem - T_SKILL0);
        }
    }
    if (!l_toggles()) g_l_held = 0;
    if (g_l_held) b |= PSP_CTRL_LTRIGGER;
    if (g_target_tap_until && SDL_GetTicks() < g_target_tap_until) b |= PSP_CTRL_LTRIGGER | PSP_CTRL_DOWN;
    else g_target_tap_until = 0;
    g_touch_buttons = b;
    {
        uint64_t held = 0;
        for (int i = 0; i < FINGERS; i++)
            if (g_fingers[i].elem > 0 && g_fingers[i].elem < 64) held |= 1ull << g_fingers[i].elem;
        g_held_elems = held;
    }
    g_stick_run = run;
    g_skill_down = sd;
    g_attack_held = atk;
    g_touch_ax = ax;
    g_touch_ay = ay;
    g_touch_rx = rx;
    g_touch_ry = ry;
    g_touch_rheld = rheld;
    SDL_SetAtomicInt(&g_touch_btn_atomic, (int)b);
    SDL_SetAtomicInt(&g_touch_stick_atomic, ax < 0 ? -1 : (ax | ay << 8));
}

static void touch_release_all(void) {
    memset(g_fingers, 0, sizeof g_fingers);
    g_touch_buttons = 0;
    g_held_elems = 0;
    g_skill_down = 0;
    g_attack_held = 0;
    g_touch_ax = g_touch_ay = -1;
    g_touch_rx = g_touch_ry = 0;
    g_touch_rheld = 0;
    SDL_SetAtomicInt(&g_touch_btn_atomic, 0);
    SDL_SetAtomicInt(&g_touch_stick_atomic, -1);
}

/* The colours, at the chosen opacity. */
static uint32_t va(int r, int g, int b, int a) { return rgba(r, g, b, a * g_vpad_alpha / 100); }

static void draw_pill(canvas *c, float s, const layout *L, int g, uint32_t bit, const char *label, uint32_t fillc, uint32_t on, uint32_t line, uint32_t white, float t) {
    float hw, hh;
    pill_size(L, g, &hw, &hh);
    (void)bit;
    const int pe = g == G_L ? T_L : g == G_R ? T_R : g == G_SELECT ? T_SELECT : T_START;
    round_rect(c, (L->x[g] - hw) * s, (L->y[g] - hh) * s, 2 * hw * s, 2 * hh * s, hh * s, elem_held(pe) ? on : fillc, t, line);
    text_centered(c, L->x[g] * s, L->y[g] * s, (int)(hh * (g == G_L || g == G_R ? 1.0f : 0.95f) * s), white, label);
}

/* A face button's symbol at (cx, cy), half size k, in pixels: 0 cross,
 * 1 circle, 2 square, 3 triangle. */
static void face_shape(canvas *c, float cx, float cy, float k, int shape, float t, uint32_t sc) {
    if (shape == 0) { segment(c, cx - k, cy - k, cx + k, cy + k, t, sc); segment(c, cx - k, cy + k, cx + k, cy - k, t, sc); }
    else if (shape == 1) ring(c, cx, cy, k, t, sc);
    else if (shape == 2) {
        segment(c, cx - k, cy - k, cx + k, cy - k, t, sc); segment(c, cx + k, cy - k, cx + k, cy + k, t, sc);
        segment(c, cx + k, cy + k, cx - k, cy + k, t, sc); segment(c, cx - k, cy + k, cx - k, cy - k, t, sc);
    } else {
        segment(c, cx, cy - k * 1.1f, cx + k, cy + k * 0.7f, t, sc); segment(c, cx + k, cy + k * 0.7f, cx - k, cy + k * 0.7f, t, sc);
        segment(c, cx - k, cy + k * 0.7f, cx, cy - k * 1.1f, t, sc);
    }
}

/* The buttons' pictures at (cx, cy), half size k, in pixels: lines and rings
 * only, drawn here (none of the game's art). */
static void poly(canvas *c, float cx, float cy, float k, const float *pt, int n, int closed, float t, uint32_t col) {
    for (int i = 0; i + 1 < n + closed; i++) {
        const int j = (i + 1) % n;
        segment(c, cx + pt[2 * i] * k, cy + pt[2 * i + 1] * k, cx + pt[2 * j] * k, cy + pt[2 * j + 1] * k, t, col);
    }
}
/* An arc from angle a0 to a1 (radians, 0 = right, clockwise on the screen). */
static void arc(canvas *c, float cx, float cy, float r, float a0, float a1, float t, uint32_t col) {
    const int n = 12;
    for (int i = 0; i < n; i++) {
        const float u = a0 + (a1 - a0) * i / n, v = a0 + (a1 - a0) * (i + 1) / n;
        segment(c, cx + r * cosf(u), cy + r * sinf(u), cx + r * cosf(v), cy + r * sinf(v), t, col);
    }
}
/* A filled polygon (pixel points, at most 32), anti-aliased: even-odd inside,
 * coverage from the distance to the nearest edge. */
static void fpoly_px(canvas *c, const float *p, int n, uint32_t col) {
    float x0 = p[0], x1 = p[0], y0 = p[1], y1 = p[1];
    for (int i = 1; i < n; i++) {
        x0 = fminf(x0, p[2 * i]); x1 = fmaxf(x1, p[2 * i]);
        y0 = fminf(y0, p[2 * i + 1]); y1 = fmaxf(y1, p[2 * i + 1]);
    }
    for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            int in = 0;
            float d2 = 1e30f;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const float ax = p[2 * j], ay = p[2 * j + 1], bx = p[2 * i], by = p[2 * i + 1];
                if ((ay > qy) != (by > qy) && qx < ax + (qy - ay) * (bx - ax) / (by - ay)) in = !in;
                const float ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
                float u = l2 > 0 ? ((qx - ax) * ex + (qy - ay) * ey) / l2 : 0;
                u = u < 0 ? 0 : u > 1 ? 1 : u;
                const float dx = qx - ax - u * ex, dy = qy - ay - u * ey;
                d2 = fminf(d2, dx * dx + dy * dy);
            }
            const int k = cov(in ? -sqrtf(d2) : sqrtf(d2));
            if (k) blend(c, x, y, col, k);
        }
}
/* The same in an icon's units (-1..1 around (cx, cy), times k), scaled by sc. */
static void fpoly(canvas *c, float cx, float cy, float k, const float *pt, int n, float sc, uint32_t col) {
    float p[64];
    for (int i = 0; i < n && i < 32; i++) { p[2 * i] = cx + pt[2 * i] * k * sc; p[2 * i + 1] = cy + pt[2 * i + 1] * k * sc; }
    fpoly_px(c, p, n < 32 ? n : 32, col);
}
/* A blade from its base (ax, ay) to its point (bx, by), half width w; icon units. */
static void blade(canvas *c, float cx, float cy, float k, float ax, float ay, float bx, float by, float w, uint32_t col) {
    const float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy), nx = -dy / l * w, ny = dx / l * w;
    const float sx = ax + dx * 0.78f, sy = ay + dy * 0.78f;
    const float p[] = { ax + nx, ay + ny, sx + nx, sy + ny, bx, by, sx - nx, sy - ny, ax - nx, ay - ny };
    fpoly(c, cx, cy, k, p, 5, 1, col);
}
/* A thick stroke in icon units. */
static void bar(canvas *c, float cx, float cy, float k, float x0, float y0, float x1, float y1, float w, uint32_t col) {
    segment(c, cx + x0 * k, cy + y0 * k, cx + x1 * k, cy + y1 * k, w * k, col);
}
/* A filled arrow head at (x, y) pointing (dx, dy), icon units. */
static void head(canvas *c, float cx, float cy, float k, float x, float y, float dx, float dy, float len, float w, uint32_t col) {
    const float p[] = { x + dx * len, y + dy * len, x - dy * w, y + dx * w, x + dy * w, y - dx * w };
    fpoly(c, cx, cy, k, p, 3, 1, col);
}

/* Each picture's colour. */
static const unsigned char k_icon_rgb[][3] = {
    [ICON_CROUCH] = { 235, 225, 205 }, [ICON_DODGE] = { 150, 205, 255 }, [ICON_RUN] = { 235, 225, 205 },
    [ICON_SHEATHE] = { 240, 228, 200 }, [ICON_ITEM] = { 150, 235, 170 }, [ICON_SWORD] = { 250, 245, 235 },
    [ICON_GUARD] = { 175, 205, 240 }, [ICON_AIM] = { 140, 232, 160 }, [ICON_TARGET] = { 255, 205, 110 },
    [ICON_DEMON] = { 255, 105, 75 }, [ICON_SPIRIT] = { 130, 210, 255 }, [ICON_DANCE] = { 255, 150, 115 },
    [ICON_FADE] = { 195, 175, 255 }, [ICON_CHARGE] = { 255, 180, 85 }, [ICON_TALK] = { 250, 245, 235 },
    [ICON_CHEST] = { 240, 200, 120 }, [ICON_DOOR] = { 235, 225, 205 }, [ICON_CHAIR] = { 225, 190, 150 },
    [ICON_BOARD] = { 240, 228, 200 }, [ICON_QUEST] = { 255, 205, 110 }, [ICON_TRAIN] = { 225, 190, 150 }, [ICON_DELIVERY] = { 240, 200, 120 }, [ICON_CARVE] = { 235, 225, 205 }, [ICON_SLEEP] = { 190, 200, 255 }, [ICON_BAG] = { 240, 200, 120 }, [ICON_KICK] = { 235, 225, 205 },
    [ICON_CHAT] = { 250, 245, 235 }, [ICON_DRAW] = { 250, 245, 235 }, [ICON_USE] = { 150, 235, 170 },
    [ICON_SORT] = { 235, 225, 205 }, [ICON_DELETE] = { 255, 150, 130 }, [ICON_ZOOM] = { 235, 225, 205 },
    [ICON_NEEDS] = { 240, 228, 200 }, [ICON_SAVE] = { 150, 205, 255 }, [ICON_PICK] = { 150, 235, 170 }, [ICON_VIEW] = { 235, 225, 205 },
};
static uint32_t icon_col(int which, int a) { return va(k_icon_rgb[which][0], k_icon_rgb[which][1], k_icon_rgb[which][2], a); }

/* The pictures from game-icons.net (tools/touch_icons.py), drawn over a
 * square of 2k around (cx, cy), filtered; 0 when `which` has none. */
#include "touch_icons.h"
static int mask_icon(canvas *c, float cx, float cy, float k, int which, uint32_t col) {
    for (size_t n = 0; n < sizeof k_touch_icons / sizeof k_touch_icons[0]; n++) {
        if (k_touch_icons[n].icon != which) continue;
        const unsigned char *m = k_touch_icons[n].a;
        const int S = TOUCH_ICON_SIZE;
        const float side = 2 * k, step = S / side;
        for (int y = (int)(cy - k); y <= (int)(cy + k); y++)
            for (int x = (int)(cx - k); x <= (int)(cx + k); x++) {
                const float u = (x + 0.5f - (cx - k)) * step - 0.5f, v = (y + 0.5f - (cy - k)) * step - 0.5f;
                if (u < -0.5f || v < -0.5f || u > S - 0.5f || v > S - 0.5f) continue;
                const int i0 = (int)floorf(u), j0 = (int)floorf(v);
                const float fu = u - i0, fv = v - j0;
                float sum = 0;
                for (int dj = 0; dj < 2; dj++)
                    for (int di = 0; di < 2; di++) {
                        const int i = i0 + di, j = j0 + dj;
                        if (i < 0 || j < 0 || i >= S || j >= S) continue;
                        sum += m[j * S + i] * (di ? fu : 1 - fu) * (dj ? fv : 1 - fv);
                    }
                if (sum >= 1) blend(c, x, y, col, (int)sum);
            }
        return 1;
    }
    return 0;
}

/* The buttons' pictures at (cx, cy), half size k, in pixels: game-icons.net's
 * where there is one, else filled shapes drawn here (none of the game's art);
 * `cut` is the button's dark, for the details cut into them. */
static void icon(canvas *c, float cx, float cy, float k, int which, float t, uint32_t col) {
    if (mask_icon(c, cx, cy, k * 1.08f, which, col)) return;
    const uint32_t cut = va(22, 16, 11, 235);
    switch (which) {
    case ICON_CROUCH: {                                /* down, onto the ground */
        static const float a[] = { -1, -0.7f, 0, 0.1f, 1, -0.7f }, g[] = { -1.1f, 0.7f, 1.1f, 0.7f };
        poly(c, cx, cy, k, a, 3, 0, t, col);
        poly(c, cx, cy, k, g, 2, 0, t, col);
        break;
    }
    case ICON_RUN: {                                   /* two chevrons forward */
        static const float a[] = { -0.7f, -0.7f, -0.1f, 0, -0.7f, 0.7f }, b[] = { 0.1f, -0.7f, 0.7f, 0, 0.1f, 0.7f };
        poly(c, cx, cy, k, a, 3, 0, t, col);
        poly(c, cx, cy, k, b, 3, 0, t, col);
        break;
    }
    case ICON_ITEM: {                                  /* a potion */
        static const float body[] = { -0.22f, -0.6f, -0.22f, -0.25f, -0.62f, 0.12f, -0.62f, 0.8f, -0.45f, 0.98f,
                                      0.45f, 0.98f, 0.62f, 0.8f, 0.62f, 0.12f, 0.22f, -0.25f, 0.22f, -0.6f };
        fpoly(c, cx, cy, k, body, 10, 1, col);
        bar(c, cx, cy, k, -0.32f, -0.8f, 0.32f, -0.8f, 0.2f, col);
        bar(c, cx, cy, k, -0.35f, 0.3f, -0.35f, 0.65f, 0.1f, cut);
        break;
    }
    case ICON_DODGE: {                                 /* a roll: a ball and the arrow round it */
        const float a0 = 2.6f, a1 = 2.6f + 4.4f, r = 0.68f;
        arc(c, cx, cy, r * k, a0, a1, 0.2f * k, col);
        const float ex = r * cosf(a1), ey = r * sinf(a1);
        head(c, cx, cy, k, ex, ey, -sinf(a1), cosf(a1), 0.42f, 0.32f, col);
        disc(c, cx, cy, 0.24f * k, col);
        for (int i = 0; i < 3; i++)                    /* speed lines */
            bar(c, cx, cy, k, -1.05f, 0.35f + i * 0.22f, -0.75f + i * 0.08f, 0.35f + i * 0.22f, 0.09f, col);
        break;
    }
    case ICON_SHEATHE: {                               /* a sword's hilt over its sheath */
        static const float sheath[] = { -0.36f, 0.02f, 0.36f, 0.02f, 0.3f, 0.78f, 0, 1.05f, -0.3f, 0.78f };
        fpoly(c, cx, cy, k, sheath, 5, 1, col);
        bar(c, cx, cy, k, 0, 0.12f, 0, 0.75f, 0.09f, cut);
        bar(c, cx, cy, k, -0.62f, -0.2f, 0.62f, -0.2f, 0.2f, col);
        bar(c, cx, cy, k, 0, -0.25f, 0, -0.82f, 0.2f, col);
        disc(c, cx, cy - 0.92f * k, 0.15f * k, col);
        head(c, cx, cy, k, 0.8f, 0.3f, 0, 1, 0.35f, 0.22f, col);   /* down it goes */
        bar(c, cx, cy, k, 0.8f, -0.45f, 0.8f, 0.35f, 0.1f, col);
        break;
    }
    case ICON_GUARD: {                                 /* a shield with a crest */
        static const float s[] = { -0.82f, -0.82f, 0, -1.0f, 0.82f, -0.82f, 0.82f, 0.02f, 0.46f, 0.6f, 0, 0.98f,
                                   -0.46f, 0.6f, -0.82f, 0.02f };
        fpoly(c, cx, cy, k, s, 8, 1, col);
        fpoly(c, cx, cy + 0.02f * k, k, s, 8, 0.7f, cut);
        fpoly(c, cx, cy + 0.04f * k, k, s, 8, 0.42f, col);
        break;
    }
    case ICON_AIM:                                     /* cross hairs */
        ring(c, cx, cy, k * 0.62f, 0.16f * k, col);
        bar(c, cx, cy, k, -1.05f, 0, -0.35f, 0, 0.16f, col); bar(c, cx, cy, k, 0.35f, 0, 1.05f, 0, 0.16f, col);
        bar(c, cx, cy, k, 0, -1.05f, 0, -0.35f, 0.16f, col); bar(c, cx, cy, k, 0, 0.35f, 0, 1.05f, 0.16f, col);
        disc(c, cx, cy, k * 0.14f, col);
        break;
    case ICON_TARGET: {                                /* a lock-on frame round a diamond */
        static const float d[] = { 0, -0.42f, 0.42f, 0, 0, 0.42f, -0.42f, 0 };
        fpoly(c, cx, cy, k, d, 4, 1, col);
        fpoly(c, cx, cy, k, d, 4, 0.45f, cut);
        for (int i = 0; i < 4; i++) {
            const float sx = i & 1 ? 1.f : -1.f, sy = i & 2 ? 1.f : -1.f;
            bar(c, cx, cy, k, sx * 0.95f, sy * 0.95f, sx * 0.5f, sy * 0.95f, 0.18f, col);
            bar(c, cx, cy, k, sx * 0.95f, sy * 0.95f, sx * 0.95f, sy * 0.5f, 0.18f, col);
        }
        break;
    }
    case ICON_DEMON: {                                 /* a flame with a hot core */
        static const float a[] = { 0, -1.05f, 0.24f, -0.6f, 0.5f, -0.88f, 0.56f, -0.4f, 0.82f, -0.05f, 0.8f, 0.45f,
                                   0.52f, 0.86f, 0, 1.0f, -0.52f, 0.86f, -0.8f, 0.45f, -0.82f, -0.05f, -0.46f, -0.36f,
                                   -0.36f, -0.78f, -0.12f, -0.5f },
                           b[] = { 0, -0.15f, 0.22f, 0.2f, 0.4f, 0.5f, 0.22f, 0.82f, 0, 0.88f, -0.22f, 0.82f,
                                   -0.4f, 0.5f, -0.2f, 0.25f };
        fpoly(c, cx, cy, k, a, 14, 1, col);
        fpoly(c, cx, cy, k, b, 8, 1, va(255, 222, 130, 240));
        break;
    }
    case ICON_SPIRIT: {                                /* a katana in its glow */
        const uint32_t glow = va(200, 240, 255, 200);
        arc(c, cx, cy - 0.3f * k, 0.6f * k, 2.0f, 4.3f, 0.1f * k, glow);
        arc(c, cx, cy - 0.3f * k, 0.6f * k, -1.16f, 1.14f, 0.1f * k, glow);
        arc(c, cx, cy - 0.3f * k, 0.85f * k, 2.3f, 3.6f, 0.07f * k, glow);
        arc(c, cx, cy - 0.3f * k, 0.85f * k, -0.46f, 0.84f, 0.07f * k, glow);
        blade(c, cx, cy, k, 0, 0.36f, 0.05f, -1.08f, 0.14f, col);
        bar(c, cx, cy, k, -0.32f, 0.4f, 0.32f, 0.4f, 0.18f, col);
        bar(c, cx, cy, k, 0, 0.48f, 0, 0.98f, 0.18f, col);
        break;
    }
    case ICON_DANCE:                                   /* two blades crossed, spinning */
        for (int i = -1; i <= 1; i += 2) {
            blade(c, cx, cy, k, i * -0.58f, 0.58f, i * 0.92f, -0.92f, 0.13f, col);
            bar(c, cx, cy, k, i * -0.58f, 0.58f, i * -0.86f, 0.86f, 0.16f, col);
            bar(c, cx, cy, k, i * -0.78f, 0.38f, i * -0.38f, 0.78f, 0.13f, col);
        }
        arc(c, cx, cy, k * 1.02f, 0.75f, 2.39f, 0.09f * k, col);
        head(c, cx, cy, k, 1.02f * cosf(0.75f), 1.02f * sinf(0.75f), sinf(0.75f), -cosf(0.75f), 0.3f, 0.2f, col);
        break;
    case ICON_FADE:                                    /* a slash, stepping back */
        blade(c, cx, cy, k, -0.45f, 0.15f, 0.95f, -1.0f, 0.13f, col);
        bar(c, cx, cy, k, -0.45f, 0.15f, -0.76f, 0.42f, 0.16f, col);
        bar(c, cx, cy, k, -0.68f, -0.1f, -0.22f, 0.4f, 0.13f, col);
        bar(c, cx, cy, k, 0.9f, 0.8f, -0.35f, 0.8f, 0.15f, col);
        head(c, cx, cy, k, -0.35f, 0.8f, -1, 0, 0.55f, 0.3f, col);
        break;
    case ICON_CHARGE: {                                /* a hammer, charging */
        static const float h[] = { -0.88f, -0.92f, 0.5f, -0.92f, 0.58f, -0.82f, 0.58f, -0.32f, 0.5f, -0.22f,
                                   -0.88f, -0.22f, -0.96f, -0.32f, -0.96f, -0.82f };
        fpoly(c, cx, cy, k, h, 8, 1, col);
        bar(c, cx, cy, k, -0.7f, -0.57f, 0.32f, -0.57f, 0.09f, cut);
        bar(c, cx, cy, k, -0.19f, -0.2f, -0.19f, 1.0f, 0.2f, col);
        arc(c, cx + 0.3f * k, cy + 0.38f * k, 0.36f * k, -0.95f, 0.95f, 0.11f * k, col);
        arc(c, cx + 0.3f * k, cy + 0.38f * k, 0.68f * k, -0.75f, 0.75f, 0.11f * k, col);
        break;
    }
    default:                                           /* ICON_SWORD: a blade in a slash */
        arc(c, cx, cy, k * 1.08f, -0.15f, 1.6f, 0.09f * k, col);
        blade(c, cx, cy, k, -0.36f, 0.36f, 0.95f, -0.95f, 0.17f, col);
        bar(c, cx, cy, k, -0.24f, 0.24f, 0.6f, -0.6f, 0.05f, cut);
        bar(c, cx, cy, k, -0.68f, 0.04f, -0.04f, 0.68f, 0.2f, col);
        bar(c, cx, cy, k, -0.4f, 0.4f, -0.74f, 0.74f, 0.17f, col);
        disc(c, cx - 0.82f * k, cy + 0.82f * k, 0.15f * k, col);
        break;
    }
}

/* A round button: dark glass, a soft inner rim and a gold ring; pressed, it
 * takes its picture's colour. */
static void round_button(canvas *c, float cx, float cy, float r, int pressed, int which, float t, uint32_t line, uint32_t fillc) {
    disc(c, cx, cy, r, pressed ? icon_col(which, 130) : fillc);
    disc(c, cx, cy - r * 0.08f, r * 0.82f, va(255, 245, 225, pressed ? 30 : 14));
    ring(c, cx, cy, r * 0.86f, t * 0.5f, va(255, 240, 210, 45));
    ring(c, cx, cy, r, t * 1.2f, line);
}

/* A chevron pointing (dx, dy) at (cx, cy), half size k. */
static void chevron(canvas *c, float cx, float cy, float dx, float dy, float k, float t, uint32_t col) {
    const float px = -dy, py = dx;
    segment(c, cx + dx * k * 0.5f, cy + dy * k * 0.5f, cx - dx * k * 0.3f + px * k, cy - dy * k * 0.3f + py * k, t, col);
    segment(c, cx + dx * k * 0.5f, cy + dy * k * 0.5f, cx - dx * k * 0.3f - px * k, cy - dy * k * 0.3f - py * k, t, col);
}

static void draw_vpad(canvas *c, float s) {
    const layout L = layout_for((float)c->w / s, (float)c->h / s);
    const uint32_t line = va(236, 194, 104, 150), fillc = va(20, 15, 10, 70), on = va(236, 194, 104, 150);
    const uint32_t white = va(250, 245, 235, 190);
    const float t = 2.2f * s;
#define S(v) ((v) * s)
    /* L, R, Select, Start */
    if (group_shown(G_L)) draw_pill(c, s, &L, G_L, PSP_CTRL_LTRIGGER, "L", fillc, on, line, white, t);
    if (group_shown(G_SELECT)) draw_pill(c, s, &L, G_SELECT, PSP_CTRL_SELECT, "SELECT", fillc, on, line, white, t);
    if (group_shown(G_R)) draw_pill(c, s, &L, G_R, PSP_CTRL_RTRIGGER, "R", fillc, on, line, white, t);
    if (group_shown(G_START)) draw_pill(c, s, &L, G_START, PSP_CTRL_START, "START", fillc, on, line, white, t);
    {
        /* hunting, sheathed: draw (Triangle), use (Square), kick (Select), the bag (L, lit while held) */
        const struct { int g; uint32_t bit; int pic; } hb[5] = {
            { G_DRAW, PSP_CTRL_TRIANGLE, ICON_DRAW }, { G_USE, PSP_CTRL_SQUARE, ICON_USE },
            { G_KICK, PSP_CTRL_SELECT, ICON_KICK }, { G_BAG, PSP_CTRL_LTRIGGER, ICON_BAG },
            { G_PICK, PSP_CTRL_CIRCLE, ICON_PICK } };
        for (int i = 0; i < 5; i++) {
            if (!group_shown(hb[i].g)) continue;
            const float rr = hunt_r(&L, hb[i].g), cx = L.x[hb[i].g], cy = L.y[hb[i].g];
            static const int he[5] = { T_DRAW, T_USE, T_KICK, T_BAG, T_PICK };
            const int lit = hb[i].g == G_BAG ? g_l_held : elem_held(he[i]);
            round_button(c, S(cx), S(cy), S(rr), lit, hb[i].pic, t, line, fillc);
            icon(c, S(cx), S(cy), S(rr * 0.58f), hb[i].pic, t * 1.2f, icon_col(hb[i].pic, 235));
        }
    }
    /* the d-pad: four arms */
    const float dr = dpad_r(&L), a = dr * 0.30f, len = dr * 0.80f;
    const struct { float ox, oy; uint32_t bit; } arms[4] = {
        { 0, -1, PSP_CTRL_UP }, { 0, 1, PSP_CTRL_DOWN }, { -1, 0, PSP_CTRL_LEFT }, { 1, 0, PSP_CTRL_RIGHT } };
    for (int i = 0; i < 4 && group_shown(G_DPAD); i++) {
        const float cx = L.x[G_DPAD] + arms[i].ox * len, cy = L.y[G_DPAD] + arms[i].oy * len;
        round_rect(c, S(cx - a), S(cy - a), S(2 * a), S(2 * a), S(a * 0.3f), elem_held(T_DPAD) && (g_touch_buttons & arms[i].bit) ? on : fillc, t, line);
        const float tx = arms[i].ox, ty = arms[i].oy, px = -ty, py = tx;
        segment(c, S(cx + tx * a * 0.4f), S(cy + ty * a * 0.4f), S(cx - tx * a * 0.2f + px * a * 0.4f), S(cy - ty * a * 0.2f + py * a * 0.4f), t, white);
        segment(c, S(cx + tx * a * 0.4f), S(cy + ty * a * 0.4f), S(cx - tx * a * 0.2f - px * a * 0.4f), S(cy - ty * a * 0.2f - py * a * 0.4f), t, white);
    }
    /* the stick */
    const float sr = stick_r(&L);
    const float scx = L.x[G_STICK], scy = L.y[G_STICK];
    if (pad_combined()) {
        /* the d-pad ring: plain, four parts (up, right, down, left) parted by
         * thin lines on the diagonals, a chevron on each (filled in while its
         * direction is held), then the stick's well inside */
        const float mid = sr * (1.0f + PAD_INNER) / 2, band = sr * (1.0f - PAD_INNER) * 0.86f;
        disc(c, S(scx), S(scy), S(sr), fillc);
        const struct { float deg; uint32_t bit; float dx, dy; } arcs[4] = {
            { -90, PSP_CTRL_UP, 0, -1 }, { 0, PSP_CTRL_RIGHT, 1, 0 }, { 90, PSP_CTRL_DOWN, 0, 1 }, { 180, PSP_CTRL_LEFT, -1, 0 } };
        for (int i = 0; i < 4; i++) {
            const int lit = elem_held(T_DPAD) && (g_touch_buttons & arcs[i].bit) != 0;
            if (lit)                                    /* the part held: filled in, the rest plain */
                for (float a = arcs[i].deg - 45; a < arcs[i].deg + 45; a += 4) {
                    const float r0 = a * 3.14159265f / 180, r1 = (a + 4) * 3.14159265f / 180;
                    segment(c, S(scx + cosf(r0) * mid), S(scy + sinf(r0) * mid), S(scx + cosf(r1) * mid),
                            S(scy + sinf(r1) * mid), S(band), on);
                }
        }
        for (int i = 0; i < 4; i++) {
            const int lit = elem_held(T_DPAD) && (g_touch_buttons & arcs[i].bit) != 0;
            const float d = (45.0f + 90.0f * i) * 3.14159265f / 180;      /* the line between two parts */
            segment(c, S(scx + cosf(d) * sr * PAD_INNER), S(scy + sinf(d) * sr * PAD_INNER), S(scx + cosf(d) * sr),
                    S(scy + sinf(d) * sr), t * 0.8f, line);
            chevron(c, S(scx + arcs[i].dx * mid), S(scy + arcs[i].dy * mid), arcs[i].dx, arcs[i].dy, S(band * 0.30f),
                    lit ? t * 2.0f : t, white);
        }
        ring(c, S(scx), S(scy), S(sr), t, line);
        disc(c, S(scx), S(scy), S(sr * PAD_INNER), va(12, 9, 6, 120));
        if (g_stick_run) {                              /* running: the well's outline glows */
            ring(c, S(scx), S(scy), S(sr * PAD_INNER + t * 1.5f), t * 3.0f, va(255, 214, 120, 110));
            ring(c, S(scx), S(scy), S(sr * PAD_INNER), t * 1.8f, va(255, 224, 140, 255));
        } else ring(c, S(scx), S(scy), S(sr * PAD_INNER), t * 0.8f, line);
        const float travel = sr * PAD_INNER * 0.55f, knob = sr * PAD_INNER * 0.48f;
        float kx = scx, ky = scy;
        if (g_touch_ax >= 0) { kx += (g_touch_ax - 128) / 127.0f * travel; ky += (g_touch_ay - 128) / 127.0f * travel; }
        disc(c, S(kx), S(ky), S(knob), g_touch_ax >= 0 ? on : va(236, 194, 104, 90));
        disc(c, S(kx), S(ky - knob * 0.12f), S(knob * 0.72f), va(255, 245, 225, 22));
        ring(c, S(kx), S(ky), S(knob), t, line);
    } else {
        disc(c, S(scx), S(scy), S(sr), fillc);
        if (g_stick_run) {
            ring(c, S(scx), S(scy), S(sr + t * 1.5f), t * 3.0f, va(255, 214, 120, 110));
            ring(c, S(scx), S(scy), S(sr), t * 1.8f, va(255, 224, 140, 255));
        } else ring(c, S(scx), S(scy), S(sr), t, line);
        float kx = scx, ky = scy;
        if (g_touch_ax >= 0) { kx += (g_touch_ax - 128) / 127.0f * sr; ky += (g_touch_ay - 128) / 127.0f * sr; }
        disc(c, S(kx), S(ky), S(sr * 0.45f), g_touch_ax >= 0 ? on : va(236, 194, 104, 90));
        ring(c, S(kx), S(ky), S(sr * 0.45f), t, line);
    }
    /* the camera swipe, while a finger does it */
    if (g_touch_rheld) {
        const float rr = L.h * 0.06f;
        ring(c, S(g_touch_rx), S(g_touch_ry), S(rr), t, line);
        disc(c, S(g_swipe_x), S(g_swipe_y), S(rr * 0.4f), on);
    }
    /* the face buttons, in the PSP's shapes and colours */
    const struct { float ox, oy; uint32_t bit; int shape; uint32_t tint; } face[4] = {
        { 0, 1, PSP_CTRL_CROSS, 0, va(140, 180, 245, 220) }, { 1, 0, PSP_CTRL_CIRCLE, 1, va(245, 120, 120, 220) },
        { -1, 0, PSP_CTRL_SQUARE, 2, va(245, 155, 215, 220) }, { 0, -1, PSP_CTRL_TRIANGLE, 3, va(120, 220, 180, 220) } };
    const float fo = face_o(&L), r = face_r(&L), k = r * 0.42f;
    for (int i = 0; i < 4 && group_shown(G_FACE); i++) {
        const float cx = L.x[G_FACE] + face[i].ox * fo, cy = L.y[G_FACE] + face[i].oy * fo;
        static const int fe[4] = { T_CROSS, T_CIRCLE, T_SQUARE, T_TRIANGLE };
        const int pressed = elem_held(fe[i]);
        disc(c, S(cx), S(cy), S(r), pressed ? on : fillc);
        ring(c, S(cx), S(cy), S(r), t, line);
        face_shape(c, S(cx), S(cy), S(k), face[i].shape, t * 1.4f, face[i].tint);
    }
    /* L held (item bar): Square (left) and Circle (right) beside it */
    if (l_side_shown()) {
        const float lr = l_side_r(&L);
        for (int k = 0; k < 2; k++) {
            float lx, ly;
            l_side_pos(&L, k, &lx, &ly);
            const int f = k ? 1 : 2;                    /* face[]: 1 Circle, 2 Square */
            disc(c, S(lx), S(ly), S(lr), elem_held(k ? T_LRIGHT : T_LLEFT) ? on : fillc);
            ring(c, S(lx), S(ly), S(lr), t, line);
            face_shape(c, S(lx), S(ly), S(lr * 0.36f), face[f].shape, t * 1.3f, face[f].tint);
            chevron(c, S(lx + (k ? 1.0f : -1.0f) * lr * 1.45f), S(ly), k ? 1.0f : -1.0f, 0, S(lr * 0.28f), t * 1.2f, white);
        }
    }
    /* Yes and No (menus, questions, the title screens), Next, or the action
     * button (Talk near an NPC, Square at an object) */
    float bx, by;
    const float br = big_r(&L);
    const uint32_t dark = va(20, 15, 10, 120), green = va(150, 210, 90, 230);
    const int edit_yn = g_edit && edit_page_has(G_YESNO), edit_act = g_edit && edit_page_has(G_ACT);
    if (asks_yes_no() || edit_yn) {
        yes_pos(&L, &bx, &by);
        const int yon = elem_held(T_YES), non = elem_held(T_NO);
        disc(c, S(bx), S(by), S(br), yon ? va(90, 190, 90, 170) : va(20, 15, 10, 110));
        ring(c, S(bx), S(by), S(br), t * 1.6f, va(120, 220, 110, 230));
        ring(c, S(bx), S(by), S(br * 0.82f), t * 0.8f, va(236, 194, 104, 120));
        segment(c, S(bx - br * 0.36f), S(by + br * 0.02f), S(bx - br * 0.08f), S(by + br * 0.30f), t * 2.4f, white);
        segment(c, S(bx - br * 0.08f), S(by + br * 0.30f), S(bx + br * 0.40f), S(by - br * 0.30f), t * 2.4f, white);
        no_pos(&L, &bx, &by);
        disc(c, S(bx), S(by), S(br), non ? va(200, 80, 70, 170) : va(20, 15, 10, 110));
        ring(c, S(bx), S(by), S(br), t * 1.6f, va(225, 90, 80, 230));
        ring(c, S(bx), S(by), S(br * 0.82f), t * 0.8f, va(236, 194, 104, 120));
        segment(c, S(bx - br * 0.32f), S(by - br * 0.32f), S(bx + br * 0.32f), S(by + br * 0.32f), t * 2.4f, white);
        segment(c, S(bx - br * 0.32f), S(by + br * 0.32f), S(bx + br * 0.32f), S(by - br * 0.32f), t * 2.4f, white);
        int wx[6];
        int nw = win_extras(wx);
        if (g_edit && edit_page_has(G_EXTRAS)) {       /* the editor: the longest row, to place */
            static const int sample[5] = { WX_PAGE_PREV, WX_LEFT, WX_SORT, WX_RIGHT, WX_PAGE_NEXT };
            nw = 5;
            for (int i = 0; i < 5; i++) wx[i] = sample[i];
        }
        for (int i = 0; i < nw; i++) {
            /* the menu's own: sort, pages, L/R, delete */
            float ex, ey;
            const float er = wx_r(&L);
            wx_pos(&L, i, nw, &ex, &ey);
            disc(c, S(ex), S(ey), S(er), elem_held(T_WX0 + i) ? on : dark);
            ring(c, S(ex), S(ey), S(er), t * 1.3f, line);
            switch (wx[i]) {
            case WX_SORT: icon(c, S(ex), S(ey), S(er * 0.55f), ICON_SORT, t, icon_col(ICON_SORT, 235)); break;
            case WX_DELETE: icon(c, S(ex), S(ey), S(er * 0.55f), ICON_DELETE, t, icon_col(ICON_DELETE, 235)); break;
            case WX_QTY: face_shape(c, S(ex), S(ey), S(er * 0.40f), face[3].shape, t * 1.4f, face[3].tint); break;
            case WX_ZOOM: icon(c, S(ex), S(ey), S(er * 0.55f), ICON_ZOOM, t, icon_col(ICON_ZOOM, 235)); break;
            case WX_NEEDS: icon(c, S(ex), S(ey), S(er * 0.55f), ICON_NEEDS, t, icon_col(ICON_NEEDS, 235)); break;
            case WX_VIEW: icon(c, S(ex), S(ey), S(er * 0.55f), ICON_VIEW, t, icon_col(ICON_VIEW, 235)); break;
            case WX_LEFT: case WX_RIGHT: {
                const float d = wx[i] == WX_RIGHT ? 1.0f : -1.0f;
                chevron(c, S(ex), S(ey), d, 0, S(er * 0.32f), t * 1.6f, white);
                break;
            }
            default: {                                  /* pages: two chevrons */
                const float d = wx[i] == WX_PAGE_NEXT ? 1.0f : -1.0f;
                chevron(c, S(ex - d * er * 0.2f), S(ey), d, 0, S(er * 0.28f), t * 1.5f, white);
                chevron(c, S(ex + d * er * 0.2f), S(ey), d, 0, S(er * 0.28f), t * 1.5f, white);
            }
            }
        }
        if (detail_shown() || edit_yn) {
            /* Square: a quest's details */
            const float dr = yn_r(&L);
            detail_pos(&L, &bx, &by);
            disc(c, S(bx), S(by), S(dr), elem_held(T_DETAIL) ? on : dark);
            ring(c, S(bx), S(by), S(dr), t * 1.3f, line);
            face_shape(c, S(bx), S(by), S(dr * 0.42f), face[2].shape, t * 1.6f, face[2].tint);
        }
    } else if (g_uictx == UI_TALK || edit_act) {
        /* Talk: the thought bubble (in the editor too, to place) */
        const float ar = act_r(&L);
        act_pos(&L, &bx, &by);
        disc(c, S(bx), S(by), S(ar), elem_held(T_ACT) ? on : dark);
        ring(c, S(bx), S(by), S(ar), t * 1.6f, green);
        icon(c, S(bx), S(by), S(ar * 0.55f), ICON_TALK, t * 1.3f, icon_col(ICON_TALK, 240));
        if (edit_act) {                                /* and where a door's second choice goes */
            act2_pos(&L, &bx, &by);
            disc(c, S(bx), S(by), S(ar * 0.8f), dark);
            const int f2 = jp_buttons() ? 0 : 1;       /* the Hall's entrance: Cross in JP/FUC, Circle else */
            ring(c, S(bx), S(by), S(ar * 0.8f), t * 1.4f, face[f2].tint);
            face_shape(c, S(bx), S(by), S(ar * 0.34f), face[f2].shape, t * 2.0f, face[f2].tint);
        }
    } else if (g_uictx == UI_OBJ) {
        const float br = act_r(&L);
        /* Square, with the object's picture; a door's second choice beside it,
         * with its PSP mark */
        act_pos(&L, &bx, &by);
        disc(c, S(bx), S(by), S(br), elem_held(T_ACT) ? on : dark);
        ring(c, S(bx), S(by), S(br), t * 1.6f, green);
        const uint32_t extra = obj_extra(g_obj_kind, g_obj_dest);
        const int pic = obj_icon(g_obj_kind);
        if (extra || pic < 0 || quest_obj(g_obj_kind) == 0 && g_obj_kind == OBJ_DOOR)   /* doors: the plain mark */
            face_shape(c, S(bx), S(by), S(br * 0.40f), face[2].shape, t * 2.0f, face[2].tint);
        else icon(c, S(bx), S(by), S(br * 0.55f), pic, t * 1.3f, icon_col(pic, 240));
        if (extra) {
            const int f = extra == PSP_CTRL_CROSS ? 0 : extra == PSP_CTRL_CIRCLE ? 1 : 3;
            act2_pos(&L, &bx, &by);
            disc(c, S(bx), S(by), S(br * 0.8f), elem_held(T_ACT2) ? on : dark);
            ring(c, S(bx), S(by), S(br * 0.8f), t * 1.4f, face[f].tint);
            face_shape(c, S(bx), S(by), S(br * 0.34f), face[f].shape, t * 2.0f, face[f].tint);
        }
    }
    if (group_shown(G_ATTACK)) {
        /* the attack pad: Triangle in the middle, Triangle + Circle up,
         * Circle to the sides, R down; the knob follows the finger */
        const float ar = attack_r(&L), x = L.x[G_ATTACK], y = L.y[G_ATTACK], sk = ar * 0.11f;
        disc(c, S(x), S(y), S(ar), fillc);
        ring(c, S(x), S(y), S(ar), t, line);
        ring(c, S(x), S(y), S(ar * 0.40f), t * 0.7f, va(236, 194, 104, 90));
        const uint32_t held = g_attack_held ? attack_bits(g_attack_kx, g_attack_ky) : 0;
        const uint32_t dim = va(250, 245, 235, 110), lit = va(255, 214, 120, 255);
        const uint32_t up = PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE;
        chevron(c, S(x), S(y - ar * 0.74f), 0, -1, S(sk * 1.6f), t * 1.4f, held == up ? lit : dim);
        chevron(c, S(x), S(y + ar * 0.74f), 0, 1, S(sk * 1.6f), t * 1.4f, held == PSP_CTRL_RTRIGGER ? lit : dim);
        chevron(c, S(x - ar * 0.76f), S(y), -1, 0, S(sk * 1.1f), t * 1.2f, held == PSP_CTRL_CIRCLE ? lit : dim);
        chevron(c, S(x + ar * 0.76f), S(y), 1, 0, S(sk * 1.1f), t * 1.2f, held == PSP_CTRL_CIRCLE ? lit : dim);
        const float kx = x + (g_attack_held ? g_attack_kx * ar : 0), ky = y + (g_attack_held ? g_attack_ky * ar : 0);
        disc(c, S(kx), S(ky), S(ar * 0.36f), g_attack_held ? on : va(30, 23, 17, 150));
        ring(c, S(kx), S(ky), S(ar * 0.36f), t * 1.3f, line);
        icon(c, S(kx), S(ky), S(ar * 0.24f), ICON_SWORD, t * 1.3f, held == PSP_CTRL_TRIANGLE ? lit : white);
    }
    {
        /* Cross (Dodge with the weapon out), Square (Sheathe), and Run (R; in
         * the lobby, in place of the R pill) */
        const int out = mobile_now() || g_stick_run;      /* drawn or running: dodge; standing: crouch */
        const struct { int g; uint32_t bit; int pic; } sm[3] = {
            { G_ROLL, PSP_CTRL_CROSS, out ? ICON_DODGE : ICON_CROUCH },
            { G_ITEM, PSP_CTRL_SQUARE, out ? ICON_SHEATHE : ICON_ITEM },
            { G_RUN, PSP_CTRL_RTRIGGER, ICON_RUN } };
        for (int i = 0; i < 3; i++) {
            if (!group_shown(sm[i].g)) continue;
            const float rr = small_r(&L, sm[i].g), cx = L.x[sm[i].g], cy = L.y[sm[i].g];
            static const int se[3] = { T_ROLL, T_ITEM, T_RUN };
            round_button(c, S(cx), S(cy), S(rr), elem_held(se[i]), sm[i].pic, t, line, fillc);
            icon(c, S(cx), S(cy), S(rr * 0.56f), sm[i].pic, t * 1.3f, icon_col(sm[i].pic, 235));
        }
    }
    /* the weapon buttons; in the editor, an empty column shows where they go */
    skill sk[SKILLS];
    const int ns = group_shown(G_SKILL) ? skills_now(sk) : 0, shown = ns ? ns : g_edit ? SKILLS : 0;
    const float kr = skill_r(&L);
    for (int i = 0; i < shown; i++) {
        float cx, cy;
        skill_pos(&L, i, shown, &cx, &cy);
        const int pic = ns ? sk[i].pic : ICON_SWORD;
        round_button(c, S(cx), S(cy), S(kr), (g_skill_down & 1 << i) != 0, pic, t, line, fillc);
        icon(c, S(cx), S(cy), S(kr * 0.58f), pic, t * 1.2f, icon_col(pic, 235));
    }
#undef S
}

/* The menu button: three bars in a gold ring. Drawn whenever the touch
 * controls could be wanted (on a phone), even with them hidden. */
static void draw_menu_button(canvas *c, float s) {
    const layout L = layout_for((float)c->w / s, (float)c->h / s);
    const float x = L.x[G_MENU] * s, y = L.y[G_MENU] * s, r = menu_r(&L) * s;
    disc(c, x, y, r, rgba(20, 15, 10, 120));
    ring(c, x, y, r, 2.2f * s, rgba(236, 194, 104, 170));
    for (int i = -1; i <= 1; i++) segment(c, x - r * 0.45f, y + i * r * 0.32f, x + r * 0.45f, y + i * r * 0.32f, 2.4f * s, rgba(250, 245, 235, 210));
    if (start_by_menu()) {                             /* hunting and the lobby: Start under it (the chat further down) */
        const float sy = y + r * 2.6f;
        disc(c, x, sy, r, rgba(20, 15, 10, 120));
        ring(c, x, sy, r, 2.2f * s, rgba(236, 194, 104, 170));
        segment(c, x - r * 0.25f, sy - r * 0.38f, x - r * 0.25f, sy + r * 0.38f, 2.6f * s, rgba(250, 245, 235, 220));
        segment(c, x - r * 0.25f, sy - r * 0.38f, x + r * 0.42f, sy, 2.6f * s, rgba(250, 245, 235, 220));
        segment(c, x - r * 0.25f, sy + r * 0.38f, x + r * 0.42f, sy, 2.6f * s, rgba(250, 245, 235, 220));
    }
    if (mhfu_chat_available()) {                       /* in a room: the chat bubble under them */
        const float cy = y + r * (start_by_menu() ? 5.2f : 2.6f);
        disc(c, x, cy, r, rgba(20, 15, 10, 120));
        ring(c, x, cy, r, 2.2f * s, rgba(236, 194, 104, 170));
        icon(c, x, cy, r * 0.6f, ICON_CHAT, 2.2f * s, icon_col(ICON_CHAT, 230));
    }
}

/* ---- the layout editor ------------------------------------------------------------------
 *
 * Every group is drawn with its bounds; touch one to pick it and drag it
 * anywhere. The toolbar at the top makes the picked one smaller or bigger,
 * resets everything to the defaults, or saves and closes (Done, or Back). */

enum { E_SMALLER, E_BIGGER, E_RESET, E_DONE, E_COUNT };
static const char *const k_edit_label[E_COUNT] = { "Smaller", "Bigger", "Reset", "Done" };
static float g_drag_dx, g_drag_dy;
static SDL_FingerID g_drag_finger;
static int g_dragging;

static void edit_toolbar(float w, float h, float *x, float *y, float *bw, float *bh, float *gap) {
    *bh = h * 0.085f;
    *bw = h * 0.26f;
    *gap = h * 0.025f;
    *x = (w - (*bw * E_COUNT + *gap * (E_COUNT - 1))) / 2;
    *y = h * 0.035f;
}

/* The page button under the toolbar, and its list (Mobile style only). */
static void edit_page_rect(float w, float h, float *x, float *y, float *pw, float *ph) {
    float tx, ty, bw, bh, gap;
    edit_toolbar(w, h, &tx, &ty, &bw, &bh, &gap);
    *pw = h * 1.05f;
    *ph = bh;
    *x = (w - *pw) / 2;
    *y = ty + bh + gap;
}

static void edit_open(void) {
    g_edit = 1;
    g_edit_sel = -1;
    g_edit_list = 0;
    g_dragging = 0;
    touch_release_all();
}

static void edit_close(void) {
    g_edit = 0;
    g_dragging = 0;
    settings_save();
}

/* A group moved to a centre (cx, cy) in window pixels, kept on screen. */
static void edit_place(const layout *L, int g, float cx, float cy) {
    float x0, y0, x1, y1;
    group_box(L, g, &x0, &y0, &x1, &y1);
    const float rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    cx = fminf(fmaxf(cx, rx), L->w - rx);
    cy = fminf(fmaxf(cy, ry), L->h - ry);
    g_place[g] = (placed){ 1, cx / L->w, cy / L->h, L->k[g] };
}

static void edit_resize(const layout *L, int g, float factor) {
    float k = L->k[g] * factor;
    k = fminf(fmaxf(k, 0.5f), 2.0f);
    g_place[g] = (placed){ 1, L->x[g] / L->w, L->y[g] / L->h, k };
}

static void edit_touch(const SDL_Event *e, float w, float h) {
    const float px = e->tfinger.x * w, py = e->tfinger.y * h;
    const layout L = layout_for(w, h);
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        float tx, ty, bw, bh, gap;
        edit_toolbar(w, h, &tx, &ty, &bw, &bh, &gap);
        if (g_style == STYLE_MOBILE) {
            float qx, qy, pw, ph;
            edit_page_rect(w, h, &qx, &qy, &pw, &ph);
            if (g_edit_list) {                         /* the open list: pick a page, or close it */
                for (int i = 0; i < EP_COUNT; i++) {
                    const float iy = qy + (i + 1) * ph * 1.05f;
                    if (px >= qx && px < qx + pw && py >= iy && py < iy + ph) { g_edit_page = i; g_edit_sel = -1; }
                }
                g_edit_list = 0;
                return;
            }
            if (px >= qx && px < qx + pw && py >= qy && py < qy + ph) { g_edit_list = 1; return; }
        }
        if (py >= ty && py < ty + bh)
            for (int i = 0; i < E_COUNT; i++) {
                const float bx = tx + i * (bw + gap);
                if (px < bx || px >= bx + bw) continue;
                if (i == E_DONE) edit_close();
                else if (i == E_RESET) { memset(g_place, 0, sizeof g_place); g_edit_sel = -1; }
                else if (g_edit_sel >= 0) edit_resize(&L, g_edit_sel, i == E_BIGGER ? 1.1f : 1 / 1.1f);
                return;
            }
        /* The group under the finger, the smallest first (so the stick does
         * not swallow the d-pad above it). */
        int best = -1;
        float best_area = 1e30f;
        for (int g = 0; g < G_COUNT; g++) {
            if (!group_shown(g)) continue;
            float x0, y0, x1, y1;
            group_box(&L, g, &x0, &y0, &x1, &y1);
            if (px >= x0 && px < x1 && py >= y0 && py < y1 && (x1 - x0) * (y1 - y0) < best_area) {
                best = g;
                best_area = (x1 - x0) * (y1 - y0);
            }
        }
        g_edit_sel = best;
        if (best >= 0) {
            g_dragging = 1;
            g_drag_finger = e->tfinger.fingerID;
            g_drag_dx = L.x[best] - px;
            g_drag_dy = L.y[best] - py;
        }
    } else if (e->type == SDL_EVENT_FINGER_MOTION && g_dragging && e->tfinger.fingerID == g_drag_finger && g_edit_sel >= 0) {
        edit_place(&L, g_edit_sel, px + g_drag_dx, py + g_drag_dy);
    } else if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && e->tfinger.fingerID == g_drag_finger) {
        g_dragging = 0;
    }
}

static void draw_editor(canvas *c, float s) {
    const float w = c->w / s, h = c->h / s;
    fill(c, 0, 0, c->w, c->h, rgba(0, 0, 0, 90));
    draw_vpad(c, s);
    draw_menu_button(c, s);
    const layout L = layout_for(w, h);
    const uint32_t gold = rgba(236, 194, 104, 255);
    for (int g = 0; g < G_COUNT; g++) {
        if (!group_shown(g)) continue;
        float x0, y0, x1, y1;
        group_box(&L, g, &x0, &y0, &x1, &y1);
        const int sel = g == g_edit_sel;
        frame_rect(c, (int)(x0 * s), (int)(y0 * s), (int)((x1 - x0) * s), (int)((y1 - y0) * s), sel ? 2 : 1,
                   sel ? gold : rgba(236, 194, 104, 110));
        if (sel) {
            diamond(c, (int)(x0 * s), (int)(y0 * s), (int)(5 * s), gold);
            diamond(c, (int)(x1 * s), (int)(y0 * s), (int)(5 * s), gold);
            diamond(c, (int)(x0 * s), (int)(y1 * s), (int)(5 * s), gold);
            diamond(c, (int)(x1 * s), (int)(y1 * s), (int)(5 * s), gold);
        }
    }
    float tx, ty, bw, bh, gap;
    edit_toolbar(w, h, &tx, &ty, &bw, &bh, &gap);
    for (int i = 0; i < E_COUNT; i++) {
        const float bx = tx + i * (bw + gap);
        const int dim = (i == E_SMALLER || i == E_BIGGER) && g_edit_sel < 0;
        round_rect(c, bx * s, ty * s, bw * s, bh * s, bh / 2 * s, i == E_DONE ? rgba(150, 60, 25, 235) : rgba(30, 23, 17, 235),
                   2 * s, dim ? rgba(126, 98, 54, 255) : gold);
        text_centered(c, (bx + bw / 2) * s, (ty + bh / 2) * s, (int)(bh * 0.40f * s),
                      dim ? rgba(150, 140, 120, 255) : rgba(252, 247, 237, 255), k_edit_label[i]);
    }
    float hint_y = ty + bh + h * 0.05f;
    if (g_style == STYLE_MOBILE) {
        /* the page: a drop-down of the button sets */
        float qx, qy, pw, ph;
        edit_page_rect(w, h, &qx, &qy, &pw, &ph);
        char label[96];
        snprintf(label, sizeof label, "%s   %s", k_edit_page[g_edit_page], g_edit_list ? "^" : "v");
        round_rect(c, qx * s, qy * s, pw * s, ph * s, ph / 2 * s, rgba(60, 40, 22, 240), 2 * s, gold);
        text_centered(c, (qx + pw / 2) * s, (qy + ph / 2) * s, (int)(ph * 0.38f * s), rgba(252, 247, 237, 255), label);
        hint_y = qy + ph + h * 0.04f;
        if (g_edit_list)
            for (int i = 0; i < EP_COUNT; i++) {
                const float iy = qy + (i + 1) * ph * 1.05f;
                round_rect(c, qx * s, iy * s, pw * s, ph * s, ph / 2 * s,
                           i == g_edit_page ? rgba(126, 90, 40, 245) : rgba(30, 23, 17, 245), 2 * s, rgba(126, 98, 54, 255));
                text_centered(c, (qx + pw / 2) * s, (iy + ph / 2) * s, (int)(ph * 0.38f * s), rgba(252, 247, 237, 255),
                              k_edit_page[i]);
            }
    }
    if (!g_edit_list)
        text_centered(c, w / 2 * s, hint_y * s, (int)(h * 0.030f * s), rgba(252, 247, 237, 230),
                      g_edit_sel >= 0 ? "Drag to move it; Smaller and Bigger resize it" : "Touch a control to move or resize it");
}

/* ---- the in-game menu: the hunter's settings board -----------------------------------------
 *
 * Opened by a swipe in from the left edge, Back (or F1), or the menu button.
 * A sheet over the whole game in the PC launcher's colours (dark leather,
 * oiled wood, tarnished brass, gold, ember): the sections as tabs along the
 * top (Display, Controls, Online, Achievements, Mods, System) and the
 * section's settings as cards in a grid, each showing its value and how it
 * changes (a switch, the choices as dots, - and +, or an arrow for what opens
 * something). A tap on a card changes it at once; the footer says what the
 * last one touched does. Achievements is the RetroAchievements set: progress,
 * a filter and every achievement with its badge. A drag scrolls; the X, Back
 * or Back again closes. Online, sign-in and the update check are Android
 * dialogs (MhfuActivity, over JNI): they need text fields. */

enum { M_NET, M_NET_CREATE, M_NET_JOIN, M_NET_LEAVE, M_ACH, M_VPAD, M_STYLE, M_PAD, M_EDIT, M_OPACITY, M_SKILLS, M_FPS, M_RES, M_SMOOTH, M_GETHREAD, M_WIDE, M_CHEATS, M_TEXPACK, M_TEXSTYLE, M_TEXINSTALL, M_MOD0, M_MOD1, M_MOD2, M_SAVES, M_FILES, M_CRASH, M_UPDATE, M_ABOUT, M_EXIT };
enum { K_TOGGLE, K_CYCLE, K_STEP, K_ACTION };
static SDL_AtomicInt g_net_busy;        /* a desktop room action is under way */
typedef struct { int id, kind, restart; const char *text, *help; } menu_item;

static const menu_item k_tab_display[] = {
    { M_RES, K_STEP, 1, "Resolution", "The 3D picture's size, a multiple of the PSP's 480x272. Higher is sharper but slower on weak phones. Applies at the next start." },
    { M_SMOOTH, K_TOGGLE, 0, "60 FPS (beta)", "Runs the game at 60 frames a second. In a room, the room plays at its slowest player's rate." },
    { M_GETHREAD, K_TOGGLE, 1, "Multi-core drawing (beta)", "Draws the picture on a second CPU core while the game runs on its own, as the PSP's graphics chip did: faster on phones with several cores. Applies at the next start." },
    { M_FPS, K_TOGGLE, 0, "Show FPS", "Shows the frame rate in the top-left corner, and the ping while you are in a room." },
};
static const menu_item k_tab_controls[] = {
    { M_VPAD, K_CYCLE, 0, "Virtual gamepad", "Automatic hides the on-screen buttons while a controller is connected. On always shows them, Off never." },
    { M_STYLE, K_CYCLE, 0, "Button style", "Mobile: the buttons follow what you can do (an attack pad with the weapon out, Yes and No in menus). Legacy PSP: the PSP's buttons everywhere." },
    { M_PAD, K_CYCLE, 0, "Stick and d-pad", "Combined: one round control, the stick in the middle and the d-pad as a ring around it. Separate: the two apart. Mobile style only." },
    { M_OPACITY, K_STEP, 0, "Button opacity", "How solid the on-screen buttons are." },
    { M_SKILLS, K_TOGGLE, 0, "Weapon buttons", "Buttons for your weapon's moves (guard, aim, charge, demon mode, spirit) while it is drawn." },
    { M_EDIT, K_ACTION, 0, "Edit button layout", "Move and resize every on-screen button: touch one to pick it, drag it, and make it smaller or bigger." },
};
#ifdef __ANDROID__
static const menu_item k_tab_online[] = {
    { M_NET, K_ACTION, 0, "Multiplayer", "Create a room or join one with its code, over the internet or the same Wi-Fi. No port forwarding." },
};
#else
/* The desktop (Linux, the Steam Deck): the room's own buttons; the code is typed in the text box. */
static const menu_item k_tab_online[] = {
    { M_NET_CREATE, K_ACTION, 0, "Create Server", "Opens a room over the internet (or on this network without a relay). Others join with its code, MH-XXXX. No port forwarding." },
    { M_NET_JOIN, K_ACTION, 0, "Join Server", "Joins a room by its code (MH-XXXX), asked for in a text box." },
    { M_NET_LEAVE, K_ACTION, 0, "Leave room", "Leaves the room you are in." },
};
#endif
static const menu_item k_tab_mods[] = {
    { M_WIDE, K_TOGGLE, 0, "Widescreen", "Widens the 3D view to the screen's shape; menus and the HUD keep the PSP's shape in the middle." },
    /* the built-in mods (mods.c), in mhfu_builtin_mods' order */
    { M_MOD0, K_TOGGLE, 0, "Target Camera", "Locks the camera on a large monster and shows a crosshair. The Target button locks on; slide it to switch." },
    { M_MOD1, K_TOGGLE, 0, "Damage Numbers", "Shows the damage of every hit as numbers over the monster." },
    { M_MOD2, K_TOGGLE, 0, "Gathering Nodes", "Shows an icon over gathering, mining and bug-catching spots." },
#if MHFU_TEXPACK
    { M_TEXPACK, K_TOGGLE, 1, "Texture pack", "Uses the installed texture pack (PPSSPP's format) in place of the game's own textures. Applies at the next start." },
    { M_TEXSTYLE, K_CYCLE, 1, "Pack style", "Some packs come in styles (button icons, layouts). A tap picks the next one. Applies at the next start." },
#endif
#if MHFU_TEXPACK && defined(__ANDROID__)
    { M_TEXINSTALL, K_ACTION, 0, "Install texture pack", "Pick a folder that holds a texture pack (with its textures.ini) to copy it into the app, or remove the one installed." },
#endif
#ifdef __ANDROID__
    { M_CHEATS, K_ACTION, 0, "Cheats", "Turn CWCheat codes on or off, or fetch the CWCheat database. Cheats pause in rooms, and achievements do not unlock while one is on." },
#endif
};
static const menu_item k_tab_system[] = {
#ifdef __ANDROID__
    { M_SAVES, K_ACTION, 0, "Save folder", "A folder of your own that your saves are kept in step with, so they outlive the app." },
    { M_FILES, K_ACTION, 0, "Change game files", "Pick another folder with the game's disc and EBOOT. The game restarts; your saves are kept." },
    { M_CRASH, K_ACTION, 0, "Copy crash log", "Copies the last crash's log, to paste into a bug report." },
    { M_UPDATE, K_ACTION, 0, "Check for updates", "Looks for a newer release on GitHub and installs it." },
    { M_ABOUT, K_ACTION, 0, "About", "The version, credits and licences." },
#endif
    { M_EXIT, K_ACTION, 0, "Exit game", "Closes the game. Save first: what is not saved is lost." },
};
typedef struct { const char *name; const menu_item *items; int n; } menu_tab;
#define TAB(name, a) { name, a, (int)(sizeof a / sizeof a[0]) }
static const menu_tab k_tabs[] = {
    TAB("Display", k_tab_display), TAB("Controls", k_tab_controls),
    TAB("Online", k_tab_online),
    { "Achievements", NULL, 0 },
    TAB("Mods", k_tab_mods), TAB("System", k_tab_system),
};
#undef TAB
#define MENU_TABS (int)(sizeof k_tabs / sizeof k_tabs[0])
#define MENU_MS   220

static uint64_t g_menu_t0;                 /* when it last started opening or closing */
static int      g_mtab = 0, g_msel = -1;   /* the section shown; the card last touched in it */
static float    g_menu_scroll;             /* how far the cards are scrolled up, window pixels */
static int      g_mdrag_on, g_mdragged;    /* a finger on the sheet; it moved enough to scroll */
static SDL_FingerID g_mdrag_id;
static float    g_mdown_x, g_mdown_y, g_mdown_scroll;
static int      g_menu_busy;               /* something on the page is still arriving: draw again */

#ifdef __ANDROID__
#include <jni.h>
/* A no-argument method of MhfuActivity: checkUpdates, openMultiplayer,
 * openAchievements -- dialogs over the game. */
static void android_call(const char *method) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, method, "()V");
    if (m) (*env)->CallVoidMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif

static void menu_set(int open) {
    if (open == g_menu_open) return;
    /* Start from wherever the slide is now. */
    const uint64_t now = SDL_GetTicks(), done = now - g_menu_t0 >= MENU_MS ? MENU_MS : now - g_menu_t0;
    g_menu_t0 = now - (MENU_MS - done);
    g_menu_open = open;
    g_mdrag_on = 0;
    if (open) touch_release_all();
}

/* How far the sheet is in, 0..1; *moving while it slides. */
static float menu_progress(int *moving) {
    const uint64_t t = SDL_GetTicks() - g_menu_t0;
    float f = t >= MENU_MS ? 1.0f : (float)t / MENU_MS;
    if (moving) *moving = t < MENU_MS;
    f = 1 - (1 - f) * (1 - f) * (1 - f);
    return g_menu_open ? f : 1 - f;
}

static const char *menu_value(int id) {
    static const char *const vp[3] = { "Automatic", "On", "Off" };
    switch (id) {
    case M_VPAD: return vp[g_vpad_mode];
    case M_FPS: return g_show_fps ? "On" : "Off";
    case M_SKILLS: return g_skills_on ? "On" : "Off";
    case M_STYLE: return g_style == STYLE_MOBILE ? "Mobile" : "Legacy PSP";
    case M_PAD: return g_pad_combined ? "Combined" : "Separate";
    case M_SMOOTH: return !MHFU_HAS_FPS60 ? "Not in this build" : !g_fps60 ? "Off" : mhfu_fps_target() >= 60 ? "On" : "On, room at 30";
    case M_WIDE: return mhfu_mod_widescreen() ? "On" : "Off";
    case M_GETHREAD: return g_gethread ? "On" : "Off";
#if MHFU_TEXPACK
    case M_TEXPACK:
        if (!g_texpack) return "Off";
        return g_texpack != g_texpack_started || mhfu_texpack_textures() > 0 ? "On" : "None installed";
    case M_TEXSTYLE: {
        static char v[300];
        if (!mhfu_texpack_styles()) return mhfu_texpack_textures() > 0 ? "Only one" : "-";
        snprintf(v, sizeof v, "%s", g_texstyle[0] ? g_texstyle : "Pack default");
        for (char *p = v; *p; p++) if (*p == '/') *p = ' ';
        return v;
    }
#endif
    case M_MOD0: case M_MOD1: case M_MOD2:
        return id - M_MOD0 >= mhfu_mod_count() ? "Not built in" : mhfu_mod_on(id - M_MOD0) ? "On" : "Off";
    case M_CHEATS: {
        static char v[24];
        if (!mhfu_cheats_active()) return "None on";
        snprintf(v, sizeof v, mhn_client_online() ? "%d on (paused)" : "%d on", mhfu_cheats_active());
        return v;
    }
    case M_RES: {
        static char r[40];
        const int want = g_res_setting ? g_res_setting : g_scale;
        snprintf(r, sizeof r, "%dx  %dp", want, PSP_H * want);
        return r;
    }
    case M_NET: {
        static char v[32];
        char code[8];
        mhfu_lobby_code(code);
        const int ms = mhn_client_ping_ms();
        if (!code[0]) return "Offline";
        if (ms >= 0) snprintf(v, sizeof v, "%s  %d ms", code, ms);
        else snprintf(v, sizeof v, "%s", code);
        return v;
    }
    case M_OPACITY: {
        static char pct[8];
        snprintf(pct, sizeof pct, "%d%%", g_vpad_alpha);
        return pct;
    }
    case M_NET_CREATE: {
        static char v[32];
        char code[8];
        mhfu_lobby_code(code);
        if (SDL_GetAtomicInt(&g_net_busy)) return "Wait...";
        if (!code[0]) return "Create";
        const int ms = mhn_client_ping_ms();
        if (ms >= 0) snprintf(v, sizeof v, "%s  %d ms", code, ms);
        else snprintf(v, sizeof v, "%s", code);
        return v;
    }
    case M_NET_JOIN: return SDL_GetAtomicInt(&g_net_busy) ? "Wait..." : "Join";
    case M_NET_LEAVE: return "Leave";
    case M_EDIT: return "Open editor";
    case M_SAVES: return "Choose folder";
    case M_FILES: return "Pick folder";
    case M_CRASH: return "Copy";
    case M_UPDATE: return "Check now";
    case M_ABOUT: return "v" MHFU_VERSION;
    case M_TEXINSTALL: return "Pick folder";
    case M_EXIT: return "";
    default: return "Open";
    }
}

/* A switch's state: 0 or 1, or -1 when it cannot be changed here. */
static int menu_on(int id) {
    switch (id) {
    case M_FPS: return g_show_fps;
    case M_SKILLS: return g_skills_on;
    case M_SMOOTH: return MHFU_HAS_FPS60 ? g_fps60 : -1;
    case M_WIDE: return mhfu_mod_widescreen() != 0;
    case M_GETHREAD: return g_gethread;
#if MHFU_TEXPACK
    case M_TEXPACK: return g_texpack;
#endif
    case M_MOD0: case M_MOD1: case M_MOD2: return id - M_MOD0 >= mhfu_mod_count() ? -1 : mhfu_mod_on(id - M_MOD0) != 0;
    default: return -1;
    }
}

/* How many choices a cycling setting has (0: unknown), and which one is on. */
static int menu_choices(int id, int *at) {
    *at = 0;
    switch (id) {
    case M_VPAD: *at = g_vpad_mode; return 3;
    case M_STYLE: *at = g_style == STYLE_MOBILE; return 2;
    case M_PAD: *at = !g_pad_combined; return 2;
#if MHFU_TEXPACK
    case M_TEXSTYLE: {
        const int n = mhfu_texpack_styles();
        for (int k = 0; k < n; k++) if (!strcmp(g_texstyle, mhfu_texpack_style(k))) *at = k + 1;
        return n ? n + 1 : 0;
    }
#endif
    default: return 0;
    }
}

/* Whether a setting differs from what the game started with. */
static int menu_pending(int id) {
    switch (id) {
    case M_RES: return g_res_setting && g_res_setting != g_scale;
    case M_GETHREAD: return g_gethread != g_gethread_started;
#if MHFU_TEXPACK
    case M_TEXPACK: return g_texpack != g_texpack_started;
    case M_TEXSTYLE: return strcmp(g_texstyle, g_texstyle_started) != 0;
#endif
    default: return 0;
    }
}

/* ---- where everything is, in window pixels ---- */

typedef struct {
    float mx, hh, fh;                      /* side margin, header and footer heights */
    float tx, tw, tabpx;                   /* the tabs: first x, each one's width, text size */
    float clx, cly, clr;                   /* the close button (round) */
    float cx, cy, cw, ch;                  /* the content */
    int   cols;                            /* cards per row */
    float gap, cardh;
} menu_geom;

static menu_geom menu_layout(float w, float h) {
    menu_geom g;
    g.mx = fmaxf(w * 0.035f, h * 0.05f);
    g.hh = h * 0.15f;
    g.fh = h * 0.12f;
    g.clr = g.hh * 0.30f;
    g.clx = w - g.mx - g.clr;
    g.cly = g.hh * 0.52f;
    g.tx = g.mx;
    const float avail = g.clx - g.clr - h * 0.04f - g.tx;
    g.tw = fminf(avail / MENU_TABS, h * 0.36f);
    g.tabpx = fminf(h * 0.036f, (g.tw - h * 0.03f) / 6.4f);
    g.cx = g.mx;
    g.cy = g.hh + h * 0.035f;
    g.cw = w - 2 * g.mx;
    g.ch = h - g.fh - h * 0.015f - g.cy;
    g.cols = w / h >= 1.9f ? 3 : 2;
    g.gap = h * 0.028f;
    g.cardh = h * 0.215f;
    return g;
}

static const menu_tab *menu_cur(void) {
    if (g_mtab < 0 || g_mtab >= MENU_TABS) g_mtab = 0;
    return &k_tabs[g_mtab];
}
static int menu_on_ach(void) { return menu_cur()->items == NULL; }

static void card_rect(const menu_geom *g, int i, int cols, float cardh, float top, float *x, float *y, float *cw) {
    *cw = (g->cw - (cols - 1) * g->gap) / cols;
    *x = g->cx + (i % cols) * (*cw + g->gap);
    *y = top + (i / cols) * (cardh + g->gap);
}

/* The - and + buttons of a stepped card. */
static void card_steps(const menu_geom *g, float x, float y, float cw, float *mxb, float *pxb, float *by, float *bs) {
    *bs = g->cardh * 0.36f;
    *by = y + g->cardh - *bs - g->cardh * 0.13f;
    *pxb = x + cw - g->cardh * 0.13f - *bs;
    *mxb = *pxb - *bs - g->cardh * 0.10f;
}

/* ---- the achievements page's data ---- */

#define ACH_MAX 400
static mhfu_ach_item *g_ach;
static mhfu_ach_game  g_ach_game;
static int            g_ach_n, g_ach_filter, g_ach_sel = -1;
static uint64_t       g_ach_at, g_ach_seen;    /* last refresh; when the page was opened */

static void ach_refresh(void) {
    if (g_ach_at && SDL_GetTicks() - g_ach_at < 1500) return;
    g_ach_at = SDL_GetTicks();
    if (!g_ach) g_ach = (mhfu_ach_item *)calloc(ACH_MAX, sizeof *g_ach);
    if (!g_ach) return;
    g_ach_n = mhfu_ach_items(&g_ach_game, g_ach, ACH_MAX);
}

/* The achievements the filter lets through, as indices into g_ach. */
static int ach_visible(int *idx) {
    int n = 0;
    for (int i = 0; i < g_ach_n; i++)
        if (g_ach_filter == 0 || (g_ach_filter == 1) == (g_ach[i].unlocked != 0)) idx[n++] = i;
    return n;
}

/* Badges, decoded once and scaled to the size they are drawn at. */
typedef struct { unsigned id; int size; uint32_t *px; uint64_t tried; } badge_pic;
#define BADGES 256
static badge_pic g_badges[BADGES];
static int       g_nbadges;

static const uint32_t *badge_get(const mhfu_ach_item *a, int size) {
    badge_pic *b = NULL;
    for (int i = 0; i < g_nbadges; i++) if (g_badges[i].id == a->id && g_badges[i].size == size) { b = &g_badges[i]; break; }
    if (!b) {
        if (g_nbadges == BADGES) {
            for (int i = 0; i < g_nbadges; i++) free(g_badges[i].px);
            g_nbadges = 0;
        }
        b = &g_badges[g_nbadges++];
        memset(b, 0, sizeof *b);
        b->id = a->id;
        b->size = size;
    }
    if (b->px) return b->px;
    if (b->tried && SDL_GetTicks() - b->tried < 2000) return NULL;
    b->tried = SDL_GetTicks();
    int w, h, n;
    unsigned char *img = stbi_load(a->badge, &w, &h, &n, 4);
    if (!img) return NULL;
    b->px = (uint32_t *)malloc((size_t)size * size * 4);
    if (b->px)
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                const unsigned char *p = img + ((size_t)(y * h / size) * w + (size_t)(x * w / size)) * 4;
                b->px[y * size + x] = rgba(p[0], p[1], p[2], p[3]);
            }
    stbi_image_free(img);
    return b->px;
}

static void blit_badge(canvas *c, int x0, int y0, int size, const uint32_t *px, int gray) {
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t v = px[y * size + x];
            if (gray) {
                const int l = ((int)(v & 0xFF) * 3 + (int)(v >> 8 & 0xFF) * 6 + (int)(v >> 16 & 0xFF)) / 10 * 6 / 10;
                v = rgba(l, l, l, (int)(v >> 24));
            }
            blend(c, x0 + x, y0 + y, v, 255);
        }
}

/* The achievements page: the summary (fixed) and the cards under it. */
static float ach_summary_h(float h) { return h * 0.22f; }
static float ach_card_h(float h) { return h * 0.165f; }
static void ach_filter_rect(const menu_geom *g, float h, int i, float *x, float *y, float *sw, float *sh) {
    *sw = h * 0.20f; *sh = h * 0.072f;
    *x = g->cx + g->cw - h * 0.03f - (3 - i) * *sw;
    *y = g->cy + h * 0.03f;
}
static void ach_account_rect(const menu_geom *g, float h, float *x, float *y, float *bw, float *bh) {
    float sx, sy, sw, sh;
    ach_filter_rect(g, h, 0, &sx, &sy, &sw, &sh);
    *x = sx; *bw = sw * 3; *bh = h * 0.062f;
    *y = g->cy + ach_summary_h(h) - h * 0.03f - *bh;
}
/* The sign-in button of the signed-out page. */
static void ach_signin_rect(const menu_geom *g, float h, float *x, float *y, float *bw, float *bh) {
    *bw = h * 0.42f; *bh = h * 0.09f;
    *x = g->cx + g->cw / 2 - *bw / 2;
    *y = g->cy + g->ch * 0.62f;
}

static float menu_content_h(const menu_geom *g, float h) {
    if (menu_on_ach()) {
        static int idx[ACH_MAX];
        const int n = g_ach_game.state == 2 ? ach_visible(idx) : 0, rows = (n + 1) / 2;
        return rows ? rows * ach_card_h(h) + (rows - 1) * g->gap : 0;
    }
    const int rows = (menu_cur()->n + g->cols - 1) / g->cols;
    return rows ? rows * g->cardh + (rows - 1) * g->gap : 0;
}
static float menu_view_h(const menu_geom *g, float h) {
    return menu_on_ach() ? g->ch - ach_summary_h(h) - g->gap : g->ch;
}
static float menu_max_scroll(float w, float h) {
    const menu_geom g = menu_layout(w, h);
    return fmaxf(0, menu_content_h(&g, h) - menu_view_h(&g, h));
}

/* ---- drawing ---- */

#define MS(v) ((int)((v) * s))
static void wfill(canvas *c, float s, float x, float y, float w, float h, uint32_t col) {
    const int iw = MS(x + w) - MS(x), ih = MS(y + h) - MS(y);
    fill(c, MS(x), MS(y), iw < 1 && w > 0 ? 1 : iw, ih < 1 && h > 0 ? 1 : ih, col);
}
static void wframe(canvas *c, float s, float x, float y, float w, float h, uint32_t col, int t) {
    frame_rect(c, MS(x), MS(y), MS(x + w) - MS(x), MS(y + h) - MS(y), t, col);
}
/* A vertical gradient, in bands (cheap enough for the CPU canvas). */
static void wgrad(canvas *c, float s, float x, float y, float w, float h, uint32_t top, uint32_t bot) {
    const int y0 = MS(y), y1 = MS(y + h), n = y1 - y0 > 0 ? y1 - y0 : 1;
    for (int j = y0; j < y1; j++) {
        const int t = (j - y0) * 255 / n;
        uint32_t col = 0;
        for (int k = 0; k < 32; k += 8)
            col |= (uint32_t)(((int)(top >> k & 0xFF) * (255 - t) + (int)(bot >> k & 0xFF) * t) / 255) << k;
        fill(c, MS(x), j, MS(x + w) - MS(x), 1, col);
    }
}

/* The launcher's colours (launcher/main.cpp). */
#define C_BG     rgba(23, 19, 15, 255)
#define C_SIDE   rgba(15, 12, 9, 255)
#define C_PANEL  rgba(34, 27, 21, 255)
#define C_FRAME  rgba(139, 107, 59, 255)
#define C_GOLD   rgba(238, 195, 102, 255)
#define C_TEXT   rgba(252, 247, 237, 255)
#define C_DIM    rgba(202, 189, 166, 255)
#define C_EMBER  rgba(153, 56, 23, 255)
#define C_EMBER2 rgba(204, 92, 33, 255)
#define C_OK     rgba(168, 216, 126, 255)
#define C_WARN   rgba(244, 193, 90, 255)
static uint32_t alpha_of(uint32_t col, int a) { return (col & 0xFFFFFFu) | (uint32_t)a << 24; }

static void draw_card(canvas *c, float s, const menu_geom *g, float h, const menu_item *it, float x, float y, float cw, int sel) {
    const float ch = g->cardh, p = h * 0.03f;
    const int exit_card = it->id == M_EXIT;
    if (exit_card) wgrad(c, s, x, y, cw, ch, C_EMBER2, rgba(110, 38, 14, 255));
    else wgrad(c, s, x, y, cw, ch, rgba(44, 35, 27, 250), rgba(30, 24, 18, 250));
    wframe(c, s, x, y, cw, ch, sel ? C_GOLD : alpha_of(C_FRAME, 150), sel ? 2 : 1);
    if (sel) {
        const int r = (int)fmaxf(2, h * 0.007f * s);
        diamond(c, MS(x), MS(y), r, C_GOLD); diamond(c, MS(x + cw), MS(y), r, C_GOLD);
        diamond(c, MS(x), MS(y + ch), r, C_GOLD); diamond(c, MS(x + cw), MS(y + ch), r, C_GOLD);
    }
    const int lpx = (int)(h * 0.033f * s), vpx = (int)(h * 0.047f * s);
    if (exit_card) {
        text_centered(c, (x + cw / 2) * s, (y + ch / 2) * s, (int)(h * 0.05f * s), C_TEXT, "EXIT GAME");
        return;
    }
    if (it->kind == K_ACTION) wfill(c, s, x, y, h * 0.008f, ch, C_EMBER2);
    text(c, MS(x + p), MS(y + p + h * 0.03f), lpx, C_DIM, it->text, MS(cw - 2 * p - h * 0.12f));
    if (it->restart && menu_pending(it->id)) {              /* applies at the next start */
        const int tpx = (int)(h * 0.024f * s);
        const char *tag = "RESTART";
        const float tw = text_width(tag, tpx) / s + h * 0.025f, th = h * 0.04f, tx = x + cw - p - tw, ty = y + p * 0.8f;
        wfill(c, s, tx, ty, tw, th, alpha_of(C_WARN, 40));
        wframe(c, s, tx, ty, tw, th, C_WARN, 1);
        text_centered(c, (tx + tw / 2) * s, (ty + th / 2) * s, tpx, C_WARN, tag);
    }
    const float base = y + ch - p - h * 0.012f, mid = y + ch - p - h * 0.03f;
    const char *v = menu_value(it->id);
    const int on = menu_on(it->id);
    if (it->kind == K_TOGGLE && on >= 0) {
        const float sw = h * 0.12f, sh = h * 0.06f, sx = x + cw - p - sw, sy = mid - sh / 2;
        round_rect(c, sx * s, sy * s, sw * s, sh * s, sh / 2 * s, on ? rgba(76, 118, 50, 255) : C_SIDE, 1.5f * s, on ? C_GOLD : C_FRAME);
        disc(c, (on ? sx + sw - sh / 2 : sx + sh / 2) * s, mid * s, sh * 0.36f * s, on ? C_TEXT : rgba(120, 104, 84, 255));
        text(c, MS(x + p), MS(base), vpx, on ? C_OK : C_DIM, v, MS(cw - 3 * p - sw));
    } else if (it->kind == K_STEP) {
        float mxb, pxb, by, bs;
        card_steps(g, x, y, cw, &mxb, &pxb, &by, &bs);
        for (int b = 0; b < 2; b++) {
            const float bx = b ? pxb : mxb, bcx = bx + bs / 2, bcy = by + bs / 2;
            wgrad(c, s, bx, by, bs, bs, rgba(62, 48, 33, 255), rgba(40, 31, 22, 255));
            wframe(c, s, bx, by, bs, bs, C_FRAME, 1);
            segment(c, (bcx - bs * 0.24f) * s, bcy * s, (bcx + bs * 0.24f) * s, bcy * s, 2.6f * s, C_GOLD);
            if (b) segment(c, bcx * s, (bcy - bs * 0.24f) * s, bcx * s, (bcy + bs * 0.24f) * s, 2.6f * s, C_GOLD);
        }
        text(c, MS(x + p), MS(base), vpx, C_GOLD, v, MS(mxb - x - 2 * p));
    } else if (it->kind == K_CYCLE) {
        int at;
        const int n = menu_choices(it->id, &at);
        float dx = x + cw - p;
        for (int k = n - 1; k >= 0; k--) {                  /* the choices, the one on lit */
            const float r = h * 0.011f;
            dx -= r * 2.6f;
            if (k == at) disc(c, (dx + r) * s, mid * s, r * 1.1f * s, C_GOLD);
            else ring(c, (dx + r) * s, mid * s, r * 0.85f * s, 1.4f * s, C_FRAME);
        }
        text(c, MS(x + p), MS(base), vpx, C_GOLD, v, MS(dx - x - 2 * p));
    } else if (it->kind == K_TOGGLE) {                     /* cannot be changed here */
        text(c, MS(x + p), MS(base), (int)(h * 0.040f * s), C_DIM, v, MS(cw - 2 * p));
    } else {
        /* opens something: the value, and an arrow */
        const float ax = x + cw - p - h * 0.01f, ar = h * 0.018f;
        segment(c, (ax - ar) * s, (mid - ar * 1.4f) * s, ax * s, mid * s, 2.6f * s, C_GOLD);
        segment(c, ax * s, mid * s, (ax - ar) * s, (mid + ar * 1.4f) * s, 2.6f * s, C_GOLD);
        text(c, MS(x + p), MS(base), (int)(h * 0.040f * s), C_TEXT, v,
             MS(cw - 3 * p - ar * 2));
    }
}

static void draw_button(canvas *c, float s, float h, float x, float y, float bw, float bh, const char *label, int ember, int on) {
    if (ember) wgrad(c, s, x, y, bw, bh, C_EMBER2, C_EMBER);
    else if (on) wgrad(c, s, x, y, bw, bh, alpha_of(C_GOLD, 90), alpha_of(C_GOLD, 40));
    else wgrad(c, s, x, y, bw, bh, rgba(52, 40, 28, 255), rgba(36, 28, 20, 255));
    wframe(c, s, x, y, bw, bh, on || ember ? C_GOLD : C_FRAME, 1);
    text_centered(c, (x + bw / 2) * s, (y + bh / 2) * s, (int)(h * 0.032f * s), on ? C_GOLD : C_TEXT, label);
}

static void draw_achievements(canvas *c, float s, const menu_geom *g, float w, float h, float oy) {
    (void)w;
    ach_refresh();
    const float p = h * 0.03f;
    if (g_ach_game.state != 2) {
        /* signed out, or the set still loading */
        const float bx = g->cx + g->cw * 0.18f, bw = g->cw * 0.64f, by = g->cy + oy + g->ch * 0.08f, bh = g->ch * 0.8f;
        wgrad(c, s, bx, by, bw, bh, rgba(44, 35, 27, 250), rgba(30, 24, 18, 250));
        wframe(c, s, bx, by, bw, bh, alpha_of(C_FRAME, 160), 1);
        text_centered(c, (bx + bw / 2) * s, (by + bh * 0.2f) * s, (int)(h * 0.055f * s), C_GOLD, "RetroAchievements");
        char unsup[300];
        snprintf(unsup, sizeof unsup,
                 "This release (%s) is not supported by RetroAchievements. Only Monster Hunter Freedom Unite (Europe) "
                 "has achievements.", g_ach_game.title);
        const char *msg = g_ach_game.state == 3 ? unsup
                        : g_ach_game.state == 1 ? "Loading the achievement set..."
                                                : "Sign in to earn achievements while you play and to see your hunter record here.";
        text_wrapped(c, MS(bx + p * 2), MS(by + bh * 0.38f), (int)(h * 0.034f * s), (int)(h * 0.034f * s * 1.35f), C_DIM, msg,
                     MS(bw - p * 4), 4);
        if (g_ach_game.state == 1) g_menu_busy |= SDL_GetTicks() - g_ach_seen < 30000;
#ifdef __ANDROID__
        if (g_ach_game.state == 0) {
            float x, y, bw2, bh2;
            ach_signin_rect(g, h, &x, &y, &bw2, &bh2);
            draw_button(c, s, h, x, y + oy, bw2, bh2, "SIGN IN", 1, 0);
        }
#else
        if (g_ach_game.state == 0)
            text_centered(c, (bx + bw / 2) * s, (by + bh * 0.8f) * s, (int)(h * 0.03f * s), C_DIM, "Sign in on the launcher's Achievements page.");
#endif
        return;
    }
    /* the summary: the set, the count, the points, a progress bar; the filter and the account */
    const float sy = g->cy + oy, sh = ach_summary_h(h);
    wgrad(c, s, g->cx, sy, g->cw, sh, rgba(48, 38, 28, 250), rgba(30, 24, 18, 250));
    wframe(c, s, g->cx, sy, g->cw, sh, alpha_of(C_FRAME, 170), 1);
    float fx, fy, fw, fh;
    ach_filter_rect(g, h, 0, &fx, &fy, &fw, &fh);
    const float lw = fx - g->cx - 3 * p;
    text(c, MS(g->cx + p), MS(sy + p + h * 0.05f), (int)(h * 0.052f * s), C_GOLD, g_ach_game.title, MS(lw));
    char line[160];
    snprintf(line, sizeof line, "%u of %u unlocked  \xC2\xB7  %u of %u points  \xC2\xB7  %s", g_ach_game.unlocked, g_ach_game.total,
             g_ach_game.points, g_ach_game.points_total, mhfu_ach_user());
    text(c, MS(g->cx + p), MS(sy + p + h * 0.11f), (int)(h * 0.032f * s), C_DIM, line, MS(lw));
    const float pby = sy + sh - p - h * 0.022f, frac = g_ach_game.total ? (float)g_ach_game.unlocked / g_ach_game.total : 0;
    wfill(c, s, g->cx + p, pby, lw, h * 0.022f, C_SIDE);
    if (frac > 0) wgrad(c, s, g->cx + p, pby, lw * frac, h * 0.022f, C_GOLD, rgba(190, 140, 60, 255));
    wframe(c, s, g->cx + p, pby, lw, h * 0.022f, alpha_of(C_FRAME, 180), 1);
    static const char *const flt[3] = { "All", "Unlocked", "Locked" };
    for (int i = 0; i < 3; i++) {
        ach_filter_rect(g, h, i, &fx, &fy, &fw, &fh);
        draw_button(c, s, h, fx, fy + oy, fw, fh, flt[i], 0, i == g_ach_filter);
    }
#ifdef __ANDROID__
    float ax, ay, aw, ah;
    ach_account_rect(g, h, &ax, &ay, &aw, &ah);
    draw_button(c, s, h, ax, ay + oy, aw, ah, "Account", 0, 0);
#endif
    /* the achievements, two to a row */
    static int idx[ACH_MAX];
    const int n = ach_visible(idx);
    const float ly = sy + sh + g->gap, lh = menu_view_h(g, h), ach = ach_card_h(h), cw = (g->cw - g->gap) / 2;
    g_clip0 = MS(ly);
    g_clip1 = MS(ly + lh);
    int missing = 0;
    for (int k = 0; k < n; k++) {
        const mhfu_ach_item *a = &g_ach[idx[k]];
        const float x = g->cx + (k % 2) * (cw + g->gap), y = ly + (k / 2) * (ach + g->gap) - g_menu_scroll;
        if (y + ach < ly || y > ly + lh) continue;
        const int sel = idx[k] == g_ach_sel;
        wgrad(c, s, x, y, cw, ach, rgba(44, 35, 27, 250), rgba(30, 24, 18, 250));
        wframe(c, s, x, y, cw, ach, sel ? C_GOLD : a->unlocked ? alpha_of(C_GOLD, 120) : alpha_of(C_FRAME, 130), sel ? 2 : 1);
        const float bs = ach - 2 * p * 0.8f, bx = x + p * 0.8f, by = y + p * 0.8f;
        const int bpx = MS(bs);
        const uint32_t *pic = bpx > 0 ? badge_get(a, bpx) : NULL;
        if (pic) blit_badge(c, MS(bx), MS(by), bpx, pic, !a->unlocked);
        else {
            missing = 1;
            wfill(c, s, bx, by, bs, bs, C_SIDE);
            diamond(c, MS(bx + bs / 2), MS(by + bs / 2), (int)(bs * 0.18f * s), C_FRAME);
        }
        wframe(c, s, bx - 1, by - 1, bs + 2, bs + 2, a->unlocked ? C_GOLD : C_FRAME, 1);
        const float tx = bx + bs + p * 0.8f, tw = x + cw - p - tx;
        char pts[24];
        snprintf(pts, sizeof pts, "%u pts", a->points);
        const int ppx = (int)(h * 0.028f * s);
        const float pw = text_width(pts, ppx) / s;
        text(c, MS(x + cw - p - pw), MS(y + p + h * 0.026f), ppx, C_DIM, pts, 0);
        text(c, MS(tx), MS(y + p + h * 0.03f), (int)(h * 0.034f * s), a->unlocked ? C_GOLD : C_TEXT, a->title, MS(tw - pw - p));
        text_wrapped(c, MS(tx), MS(y + p + h * 0.075f), (int)(h * 0.027f * s), (int)(h * 0.027f * s * 1.3f), C_DIM, a->desc,
                     MS(tw), 2);
        const char *st = a->unlocked ? "UNLOCKED" : a->progress[0] ? a->progress : "Locked";
        const int spx = (int)(h * 0.025f * s);
        text(c, MS(x + cw - p) - text_width(st, spx), MS(y + ach - p * 0.7f), spx, a->unlocked ? C_OK : C_DIM, st, 0);
    }
    g_clip0 = 0;
    g_clip1 = 1 << 30;
    if (!n)
        text_centered(c, (g->cx + g->cw / 2) * s, (ly + lh * 0.3f) * s, (int)(h * 0.034f * s), C_DIM,
                      g_ach_filter == 1 ? "Nothing unlocked yet. Go hunt!" : "Nothing here.");
    if (missing) g_menu_busy |= SDL_GetTicks() - g_ach_seen < 20000;   /* badges still coming */
}

static void draw_menu(canvas *c, float s, float prog) {
    const float w = c->w / s, h = c->h / s;
    const menu_geom g = menu_layout(w, h);
    const menu_tab *tab = menu_cur();
    const float oy = (1 - prog) * h * 0.05f;              /* rises into place */
    const int a = (int)(240 * prog);
    g_menu_busy = 0;
    fill(c, 0, 0, c->w, c->h, alpha_of(C_BG, a));
    /* the header: a darker band, the tabs, the close button */
    wgrad(c, s, 0, 0, w, g.hh, alpha_of(C_SIDE, a), alpha_of(rgba(19, 15, 11, 255), a));
    wfill(c, s, 0, g.hh - 1.5f, w, 1.5f, alpha_of(C_FRAME, (int)(200 * prog)));
    wfill(c, s, 0, g.hh + 3, w, 1, alpha_of(C_FRAME, (int)(60 * prog)));
    const int tpx = (int)(g.tabpx * s);
    for (int i = 0; i < MENU_TABS; i++) {
        const float x = g.tx + i * g.tw;
        const int on = i == g_mtab;
        if (on) {
            wgrad(c, s, x, g.hh * 0.18f, g.tw, g.hh * 0.82f - 1.5f, alpha_of(C_GOLD, 0), alpha_of(C_GOLD, 55));
            wfill(c, s, x + g.tw * 0.12f, g.hh - 4.5f, g.tw * 0.76f, 3.5f, C_GOLD);
            diamond(c, MS(x + g.tw / 2), MS(g.hh - 2.5f), (int)fmaxf(2, h * 0.007f * s), C_GOLD);
        }
        text_centered(c, (x + g.tw / 2) * s, g.hh * 0.55f * s, tpx, on ? C_GOLD : C_DIM, k_tabs[i].name);
        if (i) wfill(c, s, x, g.hh * 0.35f, 1, g.hh * 0.35f, alpha_of(C_FRAME, 90));
    }
    disc(c, g.clx * s, g.cly * s, g.clr * s, rgba(40, 31, 22, 255));
    ring(c, g.clx * s, g.cly * s, g.clr * s, 1.8f * s, C_GOLD);
    const float k = g.clr * 0.36f;
    segment(c, (g.clx - k) * s, (g.cly - k) * s, (g.clx + k) * s, (g.cly + k) * s, 3.0f * s, C_TEXT);
    segment(c, (g.clx - k) * s, (g.cly + k) * s, (g.clx + k) * s, (g.cly - k) * s, 3.0f * s, C_TEXT);

    /* the page */
    const float maxs = menu_max_scroll(w, h);
    g_menu_scroll = fminf(fmaxf(g_menu_scroll, 0), maxs);
    const char *help = NULL;
    char help_buf[400];
    if (menu_on_ach()) {
        draw_achievements(c, s, &g, w, h, oy);
        if (g_ach_sel >= 0 && g_ach_sel < g_ach_n) {
            snprintf(help_buf, sizeof help_buf, "%s: %s", g_ach[g_ach_sel].title, g_ach[g_ach_sel].desc);
            help = help_buf;
        } else
            help = "Unlocked in softcore while you play; an unlock shows in the corner with a chime. Cheats pause it.";
    } else {
        g_clip0 = MS(g.cy);
        g_clip1 = MS(g.cy + g.ch);
        for (int i = 0; i < tab->n; i++) {
            float x, y, cw;
            card_rect(&g, i, g.cols, g.cardh, g.cy + oy - g_menu_scroll, &x, &y, &cw);
            if (y + g.cardh < g.cy || y > g.cy + g.ch) continue;
            draw_card(c, s, &g, h, &tab->items[i], x, y, cw, i == g_msel);
        }
        g_clip0 = 0;
        g_clip1 = 1 << 30;
        if (g_msel >= 0 && g_msel < tab->n) help = tab->items[g_msel].help;
        else help = "Tap a card to change it. Changes are kept at once.";
    }
    if (maxs > 0) {                                       /* where in the list: a brass bar at the edge */
        const float top = menu_on_ach() ? g.cy + ach_summary_h(h) + g.gap : g.cy, vh = menu_view_h(&g, h);
        const float bar = vh * vh / (vh + maxs), by = top + (vh - bar) * g_menu_scroll / maxs;
        wfill(c, s, w - g.mx * 0.5f, by + oy, 3, bar, alpha_of(C_FRAME, 220));
    }
    /* the footer: what the last card touched does */
    const float fy = h - g.fh;
    wfill(c, s, 0, fy, w, g.fh, alpha_of(C_SIDE, a));
    wfill(c, s, 0, fy, w, 1.5f, alpha_of(C_FRAME, (int)(200 * prog)));
    diamond(c, MS(g.mx + h * 0.01f), MS(fy + g.fh * 0.36f), (int)(h * 0.008f * s), C_GOLD);
    const int hpx = (int)(h * 0.030f * s);
    const char *brand = "FREEDOM UNITE  \xC2\xB7  v" MHFU_VERSION;
    const int bpx = (int)(h * 0.026f * s);
    const float bw = text_width(brand, bpx) / s;
    text(c, MS(w - g.mx - bw), MS(fy + g.fh * 0.36f + h * 0.01f), bpx, alpha_of(C_FRAME, 255), brand, 0);
    text_wrapped(c, MS(g.mx + h * 0.035f), MS(fy + g.fh * 0.36f + h * 0.011f), hpx, (int)(hpx * 1.3f), C_DIM, help,
                 MS(w - 3 * g.mx - bw - h * 0.035f), 2);
}
#undef MS

/* What a tap on a setting does; dir is -1 or +1 for the - and + buttons, 0 for a tap on the card. */
#ifndef __ANDROID__
/* Desktop rooms (the Online tab): create, join and leave block for a few
 * seconds, so they run on a thread of their own; the answer is a toast. */
static int SDLCALL net_thread(void *arg) {
    const int id = (int)(intptr_t)arg;
    char err[160] = "", code[8];
    if (id == M_NET_CREATE) {
        mhfu_lobby_code(code);
        if (code[0]) snprintf(err, sizeof err, "Already in room %s: leave it first.", code);
        else if (mhfu_lobby_create(NULL, mhfu_lobby_has_relay(), err, sizeof err) == 0) {
            mhfu_lobby_code(code);
            mhfu_window_notify("MULTIPLAYER", "Room created", code, NULL);
            err[0] = 0;
            SDL_SetAtomicInt(&g_net_busy, 0);
            return 0;
        }
    } else if (id == M_NET_JOIN) {
        uint16_t w[16];
        int n;
        while ((n = mhfu_window_text_result(w, 16)) == -1) SDL_Delay(50);   /* the box is open */
        char typed[16] = "";
        for (int i = 0; i < n && i < 15; i++) typed[i] = (char)(w[i] < 0x80 ? toupper(w[i]) : '?');
        if (n > 0 && typed[0]) {
            if (strncmp(typed, "MH-", 3) && strlen(typed) == 4) {      /* "4K7Q" alone */
                char full[16];
                snprintf(full, sizeof full, "MH-%s", typed);
                snprintf(typed, sizeof typed, "%s", full);
            }
            mhfu_lobby_code(code);
            if (code[0]) mhfu_lobby_leave();
            if (mhfu_lobby_join(typed, NULL, err, sizeof err) == 0) {
                mhfu_window_notify("MULTIPLAYER", "Joined room", typed, NULL);
                err[0] = 0;
            }
        }
    } else {
        mhfu_lobby_code(code);
        if (code[0]) mhfu_lobby_leave();
    }
    if (err[0]) mhfu_window_notify("MULTIPLAYER", "Could not do that", err, NULL);
    SDL_SetAtomicInt(&g_net_busy, 0);
    return 0;
}

static void net_action(int id) {
    if (!SDL_CompareAndSwapAtomicInt(&g_net_busy, 0, 1)) return;
    if (id == M_NET_JOIN) {
        static const uint16_t empty[1] = {0};
        if (mhfu_window_ask_text("Room code (MH-XXXX):", empty, 7) != 0) { SDL_SetAtomicInt(&g_net_busy, 0); return; }
    }
    menu_set(0);
    SDL_Thread *t = SDL_CreateThread(net_thread, "rooms", (void *)(intptr_t)id);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&g_net_busy, 0);
}
#endif

static void menu_act(const menu_item *it, int dir) {
    switch (it->id) {
    case M_VPAD: g_vpad_mode = (g_vpad_mode + 1) % 3; settings_save(); break;
    case M_STYLE: g_style = !g_style; touch_release_all(); settings_save(); break;
    case M_PAD: g_pad_combined = !g_pad_combined; touch_release_all(); settings_save(); break;
    case M_SKILLS: g_skills_on = !g_skills_on; touch_release_all(); settings_save(); break;
    case M_FPS: g_show_fps = !g_show_fps; g_show_fps_init = g_show_fps; settings_save(); break;
    case M_SMOOTH:
        if (!MHFU_HAS_FPS60) break;
        g_fps60 = !g_fps60; mhfu_fps_set_preferred(g_fps60 ? 60 : 30); settings_save(); break;
    case M_WIDE: g_wide = !mhfu_mod_widescreen(); mhfu_mod_set_widescreen(g_wide); settings_save(); break;
    case M_GETHREAD: g_gethread = !g_gethread; settings_save(); break;
#if MHFU_TEXPACK
    case M_TEXPACK: g_texpack = !g_texpack; settings_save(); break;
    case M_TEXSTYLE: {                     /* the next style; after the last, the pack's default */
        const int n = mhfu_texpack_styles();
        if (!n) break;
        int at = -1;
        for (int k = 0; k < n; k++) if (!strcmp(g_texstyle, mhfu_texpack_style(k))) at = k;
        snprintf(g_texstyle, sizeof g_texstyle, "%s", at + 1 < n ? mhfu_texpack_style(at + 1) : "");
        settings_save();
        break;
    }
#endif
    case M_MOD0: case M_MOD1: case M_MOD2:
        if (it->id - M_MOD0 >= mhfu_mod_count()) break;
        mhfu_mod_set(it->id - M_MOD0, !mhfu_mod_on(it->id - M_MOD0));
        settings_save();
        break;
    case M_RES: {
        const int want = (g_res_setting ? g_res_setting : g_scale) + (dir ? dir : 0);
        if (dir && want >= 1 && want <= 4) { g_res_setting = want; settings_save(); }
        break;
    }
    case M_OPACITY: {
        const int a = g_vpad_alpha + dir * 25;
        if (dir && a >= 25 && a <= 100) { g_vpad_alpha = a; settings_save(); }
        break;
    }
    case M_EDIT: menu_set(0); edit_open(); break;
#ifndef __ANDROID__
    case M_NET_CREATE: case M_NET_JOIN: case M_NET_LEAVE: net_action(it->id); break;
#endif
#ifdef __ANDROID__
    case M_UPDATE: menu_set(0); android_call("checkUpdates"); break;
    case M_CHEATS: menu_set(0); android_call("openCheats"); break;
    case M_TEXINSTALL: menu_set(0); android_call("installTexturePack"); break;
    case M_NET: menu_set(0); android_call("openMultiplayer"); break;
    case M_ACH: menu_set(0); android_call("openAchievements"); break;
    case M_ABOUT: menu_set(0); android_call("openAbout"); break;
    case M_FILES: menu_set(0); android_call("changeGameFiles"); break;
    case M_CRASH: menu_set(0); android_call("copyCrashLog"); break;
    case M_SAVES: menu_set(0); android_call("chooseSaveFolder"); break;
#endif
    case M_EXIT: {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        SDL_PushEvent(&e);
        break;
    }
    }
}

static int in_rect(float px, float py, float x, float y, float w, float h, float slop) {
    return px >= x - slop && px < x + w + slop && py >= y - slop && py < y + h + slop;
}

/* A tap on the achievements page. */
static void ach_tap(const menu_geom *g, float px, float py, float h) {
    if (g_ach_game.state == 0) {
#ifdef __ANDROID__
        float x, y, bw, bh;
        ach_signin_rect(g, h, &x, &y, &bw, &bh);
        if (in_rect(px, py, x, y, bw, bh, h * 0.01f)) { menu_set(0); android_call("openAchievements"); }
#endif
        return;
    }
    if (g_ach_game.state != 2) return;
    for (int i = 0; i < 3; i++) {
        float x, y, fw, fh;
        ach_filter_rect(g, h, i, &x, &y, &fw, &fh);
        if (in_rect(px, py, x, y, fw, fh, h * 0.008f)) { g_ach_filter = i; g_menu_scroll = 0; g_ach_sel = -1; return; }
    }
#ifdef __ANDROID__
    float ax, ay, aw, ah;
    ach_account_rect(g, h, &ax, &ay, &aw, &ah);
    if (in_rect(px, py, ax, ay, aw, ah, h * 0.008f)) { menu_set(0); android_call("openAchievements"); return; }
#endif
    static int idx[ACH_MAX];
    const int n = ach_visible(idx);
    const float ly = g->cy + ach_summary_h(h) + g->gap, ach = ach_card_h(h), cw = (g->cw - g->gap) / 2;
    if (py < ly || py >= ly + menu_view_h(g, h)) return;
    for (int k = 0; k < n; k++) {
        const float x = g->cx + (k % 2) * (cw + g->gap), y = ly + (k / 2) * (ach + g->gap) - g_menu_scroll;
        if (in_rect(px, py, x, y, cw, ach, 0)) { g_ach_sel = idx[k]; return; }
    }
}

/* A tap with the sheet open. */
static void menu_tap(float px, float py, float w, float h) {
    const menu_geom g = menu_layout(w, h);
    const float dx = px - g.clx, dy = py - g.cly;
    if (dx * dx + dy * dy <= (g.clr * 1.5f) * (g.clr * 1.5f)) { menu_set(0); return; }
    if (py < g.hh) {
        const int i = (int)floorf((px - g.tx) / g.tw);
        if (px >= g.tx && i >= 0 && i < MENU_TABS && i != g_mtab) {
            g_mtab = i; g_msel = -1; g_menu_scroll = 0; g_ach_sel = -1;
            if (menu_on_ach()) { g_ach_at = 0; g_ach_seen = SDL_GetTicks(); }
        }
        return;
    }
    if (menu_on_ach()) { ach_tap(&g, px, py, h); return; }
    if (py < g.cy || py >= g.cy + g.ch) return;
    const menu_tab *tab = menu_cur();
    for (int i = 0; i < tab->n; i++) {
        float x, y, cw;
        card_rect(&g, i, g.cols, g.cardh, g.cy - g_menu_scroll, &x, &y, &cw);
        if (!in_rect(px, py, x, y, cw, g.cardh, 0)) continue;
        const menu_item *it = &tab->items[i];
        g_msel = i;
        int dir = 0;
        if (it->kind == K_STEP) {
            float mxb, pxb, by, bs;
            card_steps(&g, x, y, cw, &mxb, &pxb, &by, &bs);
            const float slop = h * 0.02f;
            if (in_rect(px, py, mxb, by, bs, bs, slop)) dir = -1;
            else if (in_rect(px, py, pxb, by, bs, bs, slop)) dir = 1;
        }
        menu_act(it, dir);
        return;
    }
}

/* A finger with the sheet open: a tap picks, a drag scrolls. */
static void menu_touch(const SDL_Event *e, float px, float py, float w, float h) {
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        if (g_mdrag_on) return;
        g_mdrag_on = 1;
        g_mdrag_id = e->tfinger.fingerID;
        g_mdragged = 0;
        g_mdown_x = px; g_mdown_y = py; g_mdown_scroll = g_menu_scroll;
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (!g_mdrag_on || e->tfinger.fingerID != g_mdrag_id) return;
        if (fabsf(py - g_mdown_y) > h * 0.025f) g_mdragged = 1;
        if (g_mdragged) g_menu_scroll = fminf(fmaxf(g_mdown_scroll - (py - g_mdown_y), 0), menu_max_scroll(w, h));
        break;
    case SDL_EVENT_FINGER_UP:
        if (!g_mdrag_on || e->tfinger.fingerID != g_mdrag_id) return;
        g_mdrag_on = 0;
        if (!g_mdragged) menu_tap(g_mdown_x, g_mdown_y, w, h);
        break;
    default:
        g_mdrag_on = 0;
        break;
    }
}

/* ---- toasts ------------------------------------------------------------------------ */

#define TOAST_MAX   8
#define TOAST_IN_MS 320
#define TOAST_ON_MS 5200

typedef struct { char head[64], title[256], text[512], badge[700]; } toast;
static toast    g_toasts[TOAST_MAX];
static int      g_ntoasts;
static uint64_t g_toast_t0;
static unsigned char *g_badge;
static int      g_badge_w, g_badge_h, g_badge_loaded;

/* How far the showing toast has slid in (0..1): 1 while one shows, 0 for
 * none, -1 when it is done. Under g_lock. */
static int toast_phase(uint64_t now, float *in) {
    if (!g_ntoasts || !g_toast_t0) return 0;
    const uint64_t t = now - g_toast_t0;
    float f;
    if (t < TOAST_IN_MS) f = (float)t / TOAST_IN_MS;
    else if (t < TOAST_IN_MS + TOAST_ON_MS) f = 1;
    else if (t < TOAST_IN_MS * 2 + TOAST_ON_MS) f = 1 - (float)(t - TOAST_IN_MS - TOAST_ON_MS) / TOAST_IN_MS;
    else return -1;
    *in = 1 - (1 - f) * (1 - f) * (1 - f);
    return 1;
}

static void draw_toast(canvas *c, int x, int y, int w, int h, const toast *t) {
    const float u = g_ui;
    const uint32_t gold = rgba(236, 194, 104, 255), brass = rgba(126, 98, 54, 255);
    fill(c, x, y, w, h, rgba(30, 23, 17, 242));
    frame_rect(c, x, y, w, h, (int)(2 * u), gold);
    frame_rect(c, x + (int)(5 * u), y + (int)(5 * u), w - (int)(10 * u), h - (int)(10 * u), 1, brass);
    const int bs = (int)(64 * u), bx = x + (int)(20 * u), by = y + (h - bs) / 2;
    if (g_badge) {
        for (int j = 0; j < bs; j++)
            for (int i = 0; i < bs; i++) {
                const unsigned char *p = &g_badge[((j * g_badge_h / bs) * g_badge_w + i * g_badge_w / bs) * 4];
                blend(c, bx + i, by + j, rgba(p[0], p[1], p[2], 255), p[3]);
            }
    } else diamond(c, bx + bs / 2, by + bs / 2, bs / 2 - (int)(6 * u), gold);
    frame_rect(c, bx - 2, by - 2, bs + 4, bs + 4, 2, gold);
    const int tx = bx + bs + (int)(18 * u), tw = x + w - (int)(16 * u) - tx;
    text(c, tx, y + (int)(29 * u), (int)(13 * u), gold, t->head, tw);
    text(c, tx, y + (int)(53 * u), (int)(20 * u), rgba(252, 247, 237, 255), t->title, tw);
    text_wrapped(c, tx, y + (int)(76 * u), (int)(15 * u), (int)(18 * u), rgba(205, 192, 168, 255), t->text, tw, 2);
}

/* ---- the text box: the game's keyboard ---------------------------------------------- */

#define TEXT_MAX 128
static char          g_tb_prompt[128], g_tb_text[TEXT_MAX * 4 + 1];
static int           g_tb_limit;
static SDL_AtomicInt g_tb_state;                  /* 0 none, 1 open, 2 done, 3 cancelled */
static float g_tb_draw_k = 1.0f;                  /* the overlay canvas's scale back to window pixels */
static int   g_tb_started;                         /* the keyboard was asked for this box */

static int utf8_len(const char *s) { int n = 0; while (*s) { utf8_next(&s); n++; } return n; }
/* Where the box's field and its Send / Close buttons were drawn, in window
 * pixels (the overlay may be drawn at half size): a phone has no Enter or
 * Escape, so they are touched; touching the field brings the keyboard back. */
static float g_tb_hit[3][4];                       /* field, send, close: x, y, w, h */
static int   g_tb_hit_ok;

static void draw_textbox(canvas *c) {
    const float u = g_ui;
    const int w = (int)(560 * u) < c->w - 40 ? (int)(560 * u) : c->w - 40, h = (int)(150 * u);
    const int x = (c->w - w) / 2, y = (int)(60 * u);
    const uint32_t gold = rgba(236, 194, 104, 255);
    fill(c, x, y, w, h, rgba(30, 23, 17, 245));
    frame_rect(c, x, y, w, h, (int)(2 * u), gold);
    text(c, x + (int)(22 * u), y + (int)(36 * u), (int)(16 * u), gold, g_tb_prompt, w - (int)(44 * u));
    fill(c, x + (int)(20 * u), y + (int)(52 * u), w - (int)(40 * u), (int)(44 * u), rgba(12, 9, 7, 255));
    frame_rect(c, x + (int)(20 * u), y + (int)(52 * u), w - (int)(40 * u), (int)(44 * u), 1, rgba(126, 98, 54, 255));
    char shown[sizeof g_tb_text + 2];
    snprintf(shown, sizeof shown, "%s%s", g_tb_text, (SDL_GetTicks() / 500) % 2 ? "|" : "");
    text(c, x + (int)(32 * u), y + (int)(82 * u), (int)(20 * u), rgba(252, 247, 237, 255), shown, w - (int)(64 * u));
    text(c, x + (int)(22 * u), y + (int)(128 * u), (int)(14 * u), rgba(205, 192, 168, 255),
         "Enter: OK    Escape: cancel", w - (int)(44 * u));
    /* Send and Close, right of the hint */
    const int bw = (int)(96 * u), bh = (int)(30 * u), by = y + h - bh - (int)(10 * u);
    const int sx = x + w - (int)(20 * u) - bw, cx = sx - (int)(10 * u) - bw;
    fill(c, cx, by, bw, bh, rgba(60, 40, 30, 255));
    frame_rect(c, cx, by, bw, bh, 1, rgba(126, 98, 54, 255));
    text(c, cx + (int)(22 * u), by + (int)(21 * u), (int)(15 * u), rgba(252, 247, 237, 255), "Close", bw);
    fill(c, sx, by, bw, bh, rgba(126, 90, 40, 255));
    frame_rect(c, sx, by, bw, bh, 1, gold);
    text(c, sx + (int)(26 * u), by + (int)(21 * u), (int)(15 * u), rgba(252, 247, 237, 255), "Send", bw);
    const float k = g_tb_draw_k;                   /* canvas -> window pixels */
    const float r[3][4] = { { (float)(x + 20 * u), (float)(y + 52 * u), (float)(w - 40 * u), (float)(44 * u) },
                            { (float)sx, (float)by, (float)bw, (float)bh }, { (float)cx, (float)by, (float)bw, (float)bh } };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) g_tb_hit[i][j] = r[i][j] * k;
    g_tb_hit_ok = 1;
}
/* A touch on the open box: 1 if it took it. */
static int textbox_touch(float px, float py) {
    if (!g_tb_hit_ok) return 0;
    int which = -1;
    for (int i = 0; i < 3; i++)
        if (px >= g_tb_hit[i][0] && px < g_tb_hit[i][0] + g_tb_hit[i][2] && py >= g_tb_hit[i][1] &&
            py < g_tb_hit[i][1] + g_tb_hit[i][3]) which = i;
    if (which < 0) return 0;
    if (which == 0) { SDL_StopTextInput(g_window); SDL_StartTextInput(g_window); return 1; }
    SDL_SetAtomicInt(&g_tb_state, which == 1 ? 2 : 3);
    SDL_StopTextInput(g_window);
    return 1;
}

/* ---- the room's chat (chat.c) ----------------------------------------------------------
 *
 * The last few lines, top-left under the FPS line, each for CHAT_SHOW_MS (all
 * of them while the text box is open). Under g_lock; the half-second stats
 * tick recomposes, which ends them. */

#define CHAT_LINES   8
#define CHAT_SHOW_MS 12000
typedef struct { char nick[48], text[256]; int self; uint64_t t; } chat_line;
static chat_line g_chat[CHAT_LINES];
static int       g_nchat;

static int chat_visible(uint64_t now, const chat_line **out) {
    const int all = SDL_GetAtomicInt(&g_tb_state) == 1;
    int n = 0;
    for (int i = 0; i < g_nchat; i++)
        if (all || now - g_chat[i].t < CHAT_SHOW_MS) out[n++] = &g_chat[i];
    return n;
}

static int chat_line_h(void) { return (int)(24 * g_ui); }

static void draw_chat(canvas *c, int x, int y, const chat_line *const *lines, int n) {
    const int lh = chat_line_h(), px = (int)(15 * g_ui);
    const int w = (int)(520 * g_ui) < c->w - x - (int)(8 * g_ui) ? (int)(520 * g_ui) : c->w - x - (int)(8 * g_ui);
    for (int i = 0; i < n; i++, y += lh) {
        const chat_line *l = lines[i];
        fill(c, x, y, w, lh, rgba(20, 15, 11, 210));
        int tx = x + (int)(8 * g_ui);
        const int base = y + lh - (int)(7 * g_ui);
        if (l->self != 2) {                             /* "Name:" in gold (green for mine) */
            char who[64];
            snprintf(who, sizeof who, "%s:", l->nick);
            const int nw = text_width(who, px) < w / 2 ? text_width(who, px) : w / 2;
            text(c, tx, base, px, l->self ? rgba(150, 225, 120, 255) : rgba(236, 194, 104, 255), who, w / 2);
            tx += nw + (int)(6 * g_ui);
        }
        text(c, tx, base, px, l->self == 2 ? rgba(205, 192, 168, 255) : rgba(252, 247, 237, 255), l->text,
             x + w - (int)(6 * g_ui) - tx);
    }
}

void mhfu_window_chat(const char *nick, const char *text, int self) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (g_nchat == CHAT_LINES) memmove(g_chat, g_chat + 1, sizeof g_chat[0] * (--g_nchat));
    chat_line *l = &g_chat[g_nchat++];
    snprintf(l->nick, sizeof l->nick, "%s", nick ? nick : "");
    snprintf(l->text, sizeof l->text, "%s", text ? text : "");
    l->self = self;
    l->t = SDL_GetTicks();
    SDL_UnlockMutex(g_lock);
    SDL_Event e;
    SDL_zero(e);
    e.type = g_ev_wake;                                  /* recompose */
    SDL_PushEvent(&e);
}

/* ---- composing the overlay (on the loop's thread) --------------------------------------- */

static char g_fps_text[96] = "FPS --";
/* SDL_WaitEventTimeout, but sleeping between looks. On Android SDL's own wait
 * kept the loop's thread at a whole core (2026-11-09, Poco X7 Pro: 97% while
 * the loop itself spent 99% of its time "waiting", and the game lagged):
 * something wakes it at once, again and again. Looking every 4 ms costs
 * nothing next to that. */
static int wait_event(SDL_Event *e, int ms) {
#ifdef __ANDROID__
    const uint64_t until = SDL_GetTicks() + (uint64_t)ms;
    for (;;) {
        if (SDL_PollEvent(e)) return 1;
        const uint64_t now = SDL_GetTicks();
        if (now >= until) return 0;
        SDL_Delay(until - now < 4 ? (uint32_t)(until - now) : 4);
    }
#else
    return SDL_WaitEventTimeout(e, ms);
#endif
}
#define COMPOSE_MS 33                                   /* the overlay's quickest redraw */
/* What the touch controls show, coarsely: the overlay is the whole screen
 * drawn on the CPU (about 30 ms on the Poco X7 Pro), so it is drawn again only
 * when this changes (the stick's knob by 8 steps of 256), or every half second
 * for the rest (the FPS line, chat). Toasts, the text box, the drawer and the
 * editor animate: always. */
typedef struct {
    int ctx, okind, odest, ax, ay, rheld, sx, sy, ah, akx, aky, skills, mobile, lobby, group, vis, w, h;
    uint32_t buttons;
} overlay_sig;
static int overlay_sig_now(overlay_sig *g) {
    memset(g, 0, sizeof *g);
    if (g_edit || g_menu_open || g_ntoasts || SDL_GetAtomicInt(&g_tb_state) == 1 || menu_progress(NULL) > 0) return 0;
    g->ctx = ui_ctx_settled();
    g->okind = g_obj_kind; g->odest = g_obj_dest;
    g->ax = g_touch_ax < 0 ? -1 : g_touch_ax / 8;
    g->ay = g_touch_ay < 0 ? -1 : g_touch_ay / 8;
    g->rheld = g_touch_rheld;
    g->sx = g_touch_rheld ? (int)(g_swipe_x / 16) : 0;
    g->sy = g_touch_rheld ? (int)(g_swipe_y / 16) : 0;
    g->ah = g_attack_held;
    g->akx = g_attack_held ? (int)(g_attack_kx * 16) : 0;
    g->aky = g_attack_held ? (int)(g_attack_ky * 16) : 0;
    g->skills = g_skill_down | psp_read8(MHFU_ADDR_WEAPON_CLASS) << 8;
    g->mobile = mobile_now();
    g->lobby = lobby_now();
    g->group = mhfu_adhoc_in_group();
    g->vis = vpad_visible();
    SDL_GetWindowSizeInPixels(g_window, &g->w, &g->h);
    g->buttons = g_touch_buttons ^ (uint32_t)g_held_elems ^ (uint32_t)(g_held_elems >> 32);
    g->rheld |= g_stick_run << 1;
    g->okind |= (g_uictx == UI_WINDOW ? box_menu() : 0) << 16;
    return 1;
}

static int compose(void) {
    if (!g_gl) return 0;
    SDL_LockMutex(g_lock);
    g_uictx = ui_ctx_settled();                          /* menu, talk, objects: which buttons show */
    {
        const int id = g_uictx == UI_WINDOW ? box_menu() : 0;
        if (id != g_win_id && id) printf("touch ui: item box screen %d\n", id);
        g_win_id = id;
    }
    if (g_uictx == UI_OBJ) {                             /* the last object in reach: walking off, its */
        int k, d;                                        /* picture stays until the button goes */
        if (obj_now(&k, &d)) { g_obj_kind = k; g_obj_dest = d; }
    }
    float in = 0;
    int showing = toast_phase(SDL_GetTicks(), &in);
    while (showing < 0) {
        memmove(g_toasts, g_toasts + 1, (size_t)(--g_ntoasts) * sizeof *g_toasts);
        stbi_image_free(g_badge);
        g_badge = NULL;
        g_badge_loaded = 0;
        g_toast_t0 = g_ntoasts ? SDL_GetTicks() : 0;
        showing = toast_phase(SDL_GetTicks(), &in);
    }
    if (showing && !g_badge_loaded) {
        g_badge_loaded = 1;
        int n;
        if (g_toasts[0].badge[0]) g_badge = stbi_load(g_toasts[0].badge, &g_badge_w, &g_badge_h, &n, 4);
    }
    const int box = SDL_GetAtomicInt(&g_tb_state) == 1;
    char line[160];                                      /* the FPS line and/or the ping */
    mhfu_stats_overlay(g_show_fps, g_fps_text, line, sizeof line);
    const int fps = line[0] && g_font_ok;
    const chat_line *chat[CHAT_LINES];
    const int nchat = g_font_ok ? chat_visible(SDL_GetTicks(), chat) : 0;
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
#ifdef __ANDROID__
    const int layer = 1;                         /* the menu button is always there on a phone */
#else
    const int layer = g_edit || g_menu_open || vpad_visible() || menu_progress(NULL) > 0;
#endif
    if (layer && ww > 0 && wh > 0) {
        /* The whole screen at half size, scaled up when drawn: the touch
         * controls, the menu button, the menu, and on top the FPS line,
         * the toast and the text box drawn at half size too. */
        const float s = 0.5f;
        canvas c = { NULL, (int)(ww * s), (int)(wh * s) };
        c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
        if (!c.px) { SDL_UnlockMutex(g_lock); return 0; }
        const float ui = g_ui;
        g_ui *= s;
        int sliding = 0;
        const float prog = menu_progress(&sliding);
        if (g_edit) draw_editor(&c, s);
        else {
            if (vpad_visible() && !g_menu_open) draw_vpad(&c, s);
            if (!g_menu_open) draw_menu_button(&c, s);
        }
        if (fps) {
            const int fpx = (int)(14 * g_ui), fw = text_width(line, fpx) + (int)(10 * g_ui), fh = fpx + (int)(8 * g_ui);
            fill(&c, 0, 0, fw, fh, rgba(0, 0, 0, 192));
            text(&c, (int)(5 * g_ui), fpx + (int)(2 * g_ui), fpx, rgba(80, 255, 80, 255), line, 0);
        }
        if (nchat) {
            /* under the menu button's row when the touch controls are up */
            int cx = (int)(6 * g_ui);
            if (vpad_visible() && !g_menu_open) {     /* right of the menu button's column */
                const layout ML = layout_for((float)ww, (float)wh);
                const int right = (int)((ML.x[G_MENU] + menu_r(&ML) * 1.5f) * s);
                if (right > cx) cx = right;
            }
            draw_chat(&c, cx, (int)((fps ? 30 : 8) * g_ui), chat, nchat);
        }
        if (showing) {
            const int tw = (int)(440 * g_ui), th = (int)(116 * g_ui);
            draw_toast(&c, c.w - tw - (int)(20 * g_ui) + (int)((1 - in) * (tw + 30 * g_ui)), (int)(20 * g_ui), tw, th, &g_toasts[0]);
        }
        if (prog > 0) draw_menu(&c, s, prog);
        g_tb_draw_k = 1.0f / s;
        if (box) draw_textbox(&c);
        g_ui = ui;
        SDL_UnlockMutex(g_lock);
        psp_render_gl_overlay_scaled(c.px, c.w, c.h, ww, wh);
        free(c.px);
        return showing || box || sliding || (g_menu_open && g_menu_busy);
    }
    const int fpx = (int)(14 * g_ui), fw = fps ? text_width(line, fpx) + (int)(10 * g_ui) : 0, fh = fps ? fpx + (int)(8 * g_ui) : 0;
    const int tw = (int)(440 * g_ui), th = (int)(116 * g_ui);
    canvas c = { NULL, fw, fh };
    if (showing) { c.w = ww; c.h = th + (int)(24 * g_ui) > c.h ? th + (int)(24 * g_ui) : c.h; }
    if (box) { c.w = ww; c.h = (int)(230 * g_ui) > c.h ? (int)(230 * g_ui) : c.h; }
    const int chy = fh + (int)(6 * g_ui);
    if (nchat) { c.w = ww; c.h = chy + nchat * chat_line_h() > c.h ? chy + nchat * chat_line_h() : c.h; }
    if (c.w <= 0 || c.h <= 0) { SDL_UnlockMutex(g_lock); psp_render_gl_overlay(NULL, 0, 0); return 0; }
    c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
    if (!c.px) { SDL_UnlockMutex(g_lock); return 0; }
    if (fps) {
        fill(&c, 0, 0, fw, fh, rgba(0, 0, 0, 192));
        text(&c, (int)(5 * g_ui), fpx + (int)(2 * g_ui), fpx, rgba(80, 255, 80, 255), line, 0);
    }
    if (showing) draw_toast(&c, c.w - tw - (int)(20 * g_ui) + (int)((1 - in) * (tw + 30 * g_ui)), (int)(20 * g_ui), tw, th, &g_toasts[0]);
    if (nchat) draw_chat(&c, (int)(6 * g_ui), chy, chat, nchat);
    g_tb_draw_k = 1.0f;
    if (box) draw_textbox(&c);
    SDL_UnlockMutex(g_lock);
    psp_render_gl_overlay(c.px, c.w, c.h);
    free(c.px);
    return showing || box;                       /* still moving: compose again soon */
}

/* ---- keys ---------------------------------------------------------------------------- */

/* input.ini's names, by SDL scancode (the launcher's keys.cpp has the same). */
static const char *key_name(SDL_Scancode sc) {
    static char one[2];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { one[0] = (char)('a' + (sc - SDL_SCANCODE_A)); return one; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { one[0] = (char)('1' + (sc - SDL_SCANCODE_1)); return one; }
    static char f[8];
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { snprintf(f, sizeof f, "f%d", 1 + (sc - SDL_SCANCODE_F1)); return f; }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) { snprintf(f, sizeof f, "kp%d", 1 + (sc - SDL_SCANCODE_KP_1)); return f; }
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_KP_0: return "kp0";
    case SDL_SCANCODE_SPACE: return "space";
    case SDL_SCANCODE_RETURN: return "enter";
    case SDL_SCANCODE_BACKSPACE: return "backspace";
    case SDL_SCANCODE_TAB: return "tab";
    case SDL_SCANCODE_ESCAPE: return "escape";
    case SDL_SCANCODE_CAPSLOCK: return "capslock";
    case SDL_SCANCODE_LSHIFT: return "lshift";
    case SDL_SCANCODE_RSHIFT: return "rshift";
    case SDL_SCANCODE_LCTRL: return "lctrl";
    case SDL_SCANCODE_RCTRL: return "rctrl";
    case SDL_SCANCODE_LALT: return "lalt";
    case SDL_SCANCODE_RALT: return "ralt";
    case SDL_SCANCODE_UP: return "up";
    case SDL_SCANCODE_DOWN: return "down";
    case SDL_SCANCODE_LEFT: return "left";
    case SDL_SCANCODE_RIGHT: return "right";
    case SDL_SCANCODE_INSERT: return "insert";
    case SDL_SCANCODE_DELETE: return "delete";
    case SDL_SCANCODE_HOME: return "home";
    case SDL_SCANCODE_END: return "end";
    case SDL_SCANCODE_PAGEUP: return "pageup";
    case SDL_SCANCODE_PAGEDOWN: return "pagedown";
    case SDL_SCANCODE_COMMA: return "comma";
    case SDL_SCANCODE_PERIOD: return "period";
    case SDL_SCANCODE_SLASH: return "slash";
    case SDL_SCANCODE_SEMICOLON: return "semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "quote";
    case SDL_SCANCODE_MINUS: return "minus";
    case SDL_SCANCODE_EQUALS: return "equals";
    case SDL_SCANCODE_LEFTBRACKET: return "lbracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "rbracket";
    case SDL_SCANCODE_BACKSLASH: return "backslash";
    case SDL_SCANCODE_GRAVE: return "grave";
    case SDL_SCANCODE_KP_PLUS: return "kp_plus";
    case SDL_SCANCODE_KP_MINUS: return "kp_minus";
    case SDL_SCANCODE_KP_MULTIPLY: return "kp_multiply";
    case SDL_SCANCODE_KP_DIVIDE: return "kp_divide";
    case SDL_SCANCODE_KP_PERIOD: return "kp_period";
    default: return NULL;
    }
}

static unsigned char g_held[SDL_SCANCODE_COUNT];

static void keys_changed(void) {
    uint32_t b = 0;
    int st = 0;
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (!g_held[sc]) continue;
        const char *n = key_name((SDL_Scancode)sc);
        const int vk = n ? mhfu_keys_vk(n) : -1;
        if (vk < 0) continue;
        b |= mhfu_keys_buttons((unsigned)vk);
        st |= mhfu_keys_stick((unsigned)vk);
    }
    SDL_SetAtomicInt(&g_buttons, (int)b);
    SDL_SetAtomicInt(&g_stick, st);
}

/* ---- GL, on psprecomp's render thread ---------------------------------------------------- */

static int gl_init(void *user) {
    (void)user;
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) { fprintf(stderr, "window: no GL context: %s\n", SDL_GetError()); return -1; }
    SDL_GL_MakeCurrent(g_window, g_ctx);
    typedef const unsigned char *(*get_string_fn)(unsigned);
    get_string_fn get_string = (get_string_fn)(void *)SDL_GL_GetProcAddress("glGetString");
    if (get_string)
        printf("window: %s context: %s on %s\n", g_gles ? "GLES" : "GL", (const char *)get_string(0x1F02 /* GL_VERSION */),
               (const char *)get_string(0x1F01 /* GL_RENDERER */));
    /* No vsync: the game paces itself, and a blocked swap would hold up the
     * frame after it. */
    SDL_GL_SetSwapInterval(0);
    return 0;
}
static void *gl_proc(void *user, const char *name) { (void)user; return (void *)SDL_GL_GetProcAddress(name); }
static void gl_swap(void *user) {
    (void)user;
    mhfu_perfhint_thread();              /* the render thread joins the ADPF session */
    /* Android replaces the window's surface when the screen turns or the app
     * comes back; the context has to be made current on it again, here on the
     * render thread (SDL would do it on the thread that pumps events). */
    /* It is tried again every frame until it works: the event that says the
     * app is back can come before Android has the new surface ready (coming
     * back from the setup screen or the home screen left the game running
     * into a surface that was gone: a black screen). */
    /* In the background the render thread lets go of the surface (Android
     * destroys it) and waits: drawing on would go nowhere, and a context still
     * bound to the old surface is how the game came back to a black screen.
     * The game waits for its frames meanwhile, as it would paused. */
    if (SDL_GetAtomicInt(&g_background)) {
        SDL_GL_MakeCurrent(g_window, NULL);
        printf("window: in the background, drawing paused\n");
        while (SDL_GetAtomicInt(&g_background) && !SDL_GetAtomicInt(&g_closed)) SDL_Delay(50);
        printf("window: back in the foreground\n");
        SDL_SetAtomicInt(&g_remake, 1);
        return;
    }
    static int tries;
    if (SDL_GetAtomicInt(&g_remake)) {
        if (SDL_GL_MakeCurrent(g_window, g_ctx)) {
            if (tries) printf("window: GL bound to the new surface after %d tries\n", tries + 1);
            SDL_SetAtomicInt(&g_remake, 0);
            tries = 0;
        } else if (tries++ % 60 == 0) {
            printf("window: GL cannot bind the surface yet (%s)\n", SDL_GetError());
        }
    }
    if (!SDL_GL_SwapWindow(g_window)) SDL_SetAtomicInt(&g_remake, 1);
}

/* Called as each event is queued, on whichever thread queues it: Android's
 * pause and resume come on its own thread while the loop may be blocked. */
static bool SDLCALL app_events(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        SDL_SetAtomicInt(&g_background, 1);
    } else if (e->type == SDL_EVENT_WILL_ENTER_FOREGROUND || e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        SDL_SetAtomicInt(&g_background, 0);
        SDL_SetAtomicInt(&g_remake, 1);
    }
    return true;
}
static void gl_size(void *user, int *w, int *h) { (void)user; SDL_GetWindowSizeInPixels(g_window, w, h); }

void mhfu_window_size(int *w, int *h) {
    *w = *h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, w, h);
}

/* ---- the API the game calls ------------------------------------------------------------ */

void mhfu_window_options(int fullscreen, int out_w, int out_h, const char *keys_ini) {
    g_fullscreen = fullscreen;
    g_out_w = out_w;
    g_out_h = out_h;
    mhfu_keys_load(keys_ini);
}

static void wake(Uint32 type) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    SDL_PushEvent(&e);
}

int mhfu_window_start(int scale) {
    g_scale = g_res_setting ? g_res_setting : scale > 0 ? scale : 2;   /* the menu's choice wins */
    SDL_LockMutex(g_lock);
    g_window_state = 0;
    wake(g_ev_window);
    while (g_window_state == 0) SDL_WaitCondition(g_cond, g_lock);
    const int ok = g_window_state > 0;
    SDL_UnlockMutex(g_lock);
    return ok ? 0 : -1;
}

int mhfu_window_gl(void) {
    static psp_gl_host host;
    memset(&host, 0, sizeof host);
    host.gles = g_gles;
    host.init = gl_init;
    host.get_proc = gl_proc;
    host.swap = gl_swap;
    host.size = gl_size;
    if (psp_render_gl_start(&host, g_scale) != 0) return -1;
    if (psp_render_select("gl") != 0) return -1;
    g_gl = 1;
    if (g_fps60) mhfu_fps_set_preferred(60);
    if (g_wide) mhfu_mod_set_widescreen(1);
    for (int i = 0; i < mhfu_mod_count(); i++)
        if (g_mod_set[i] >= 0) mhfu_mod_set(i, g_mod_set[i]);
    wake(g_ev_wake);
    return 0;
}

int mhfu_window_closed(void) { return SDL_GetAtomicInt(&g_closed); }
void mhfu_window_on_close(void (*fn)(void)) { g_on_close = fn; }

uint32_t mhfu_window_buttons(uint8_t *ax, uint8_t *ay) {
    const int s = SDL_GetAtomicInt(&g_stick);
    *ax = (s & 1) ? 0 : (s & 2) ? 255 : 128;
    *ay = (s & 4) ? 0 : (s & 8) ? 255 : 128;
    const int t = SDL_GetAtomicInt(&g_touch_stick_atomic);
    if (t >= 0) { *ax = (uint8_t)(t & 0xFF); *ay = (uint8_t)(t >> 8); }
    return (uint32_t)SDL_GetAtomicInt(&g_buttons) | (uint32_t)SDL_GetAtomicInt(&g_touch_btn_atomic);
}

int mhfu_window_ask_text(const char *prompt, const uint16_t *initial, int limit) {
    if (!g_window || SDL_GetAtomicInt(&g_closed) || SDL_GetAtomicInt(&g_tb_state) == 1) return -1;
    SDL_LockMutex(g_lock);
    snprintf(g_tb_prompt, sizeof g_tb_prompt, "%s", prompt && *prompt ? prompt : "Enter text:");
    char *o = g_tb_text;
    for (int i = 0; initial && initial[i] && o + 4 < g_tb_text + sizeof g_tb_text; i++) {
        const unsigned c = initial[i];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 0x3F)); }
        else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
    g_tb_limit = limit > 0 && limit < TEXT_MAX ? limit : TEXT_MAX;
    SDL_SetAtomicInt(&g_tb_state, 1);
    SDL_UnlockMutex(g_lock);
    wake(g_ev_wake);
    return 0;
}

int mhfu_window_text_result(uint16_t *out, int cap) {
    const int s = SDL_GetAtomicInt(&g_tb_state);
    if (s == 1) return SDL_GetAtomicInt(&g_closed) ? -2 : -1;
    if (s != 2) return -2;
    SDL_LockMutex(g_lock);
    int n = 0;
    for (const char *p = g_tb_text; *p && n + 1 < cap;) out[n++] = (uint16_t)utf8_next(&p);
    if (cap > 0) out[n] = 0;
    SDL_UnlockMutex(g_lock);
    return n;
}

void mhfu_window_notify(const char *head, const char *title, const char *text_, const char *badge_png) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (g_ntoasts < TOAST_MAX) {
        toast *t = &g_toasts[g_ntoasts++];
        snprintf(t->head, sizeof t->head, "%s", head ? head : "");
        snprintf(t->title, sizeof t->title, "%s", title ? title : "");
        snprintf(t->text, sizeof t->text, "%s", text_ ? text_ : "");
        snprintf(t->badge, sizeof t->badge, "%s", badge_png ? badge_png : "");
        if (g_ntoasts == 1) { g_toast_t0 = SDL_GetTicks(); g_badge_loaded = 0; }
    }
    SDL_UnlockMutex(g_lock);
    wake(g_ev_wake);
}

/* ---- the loop, on the main thread ------------------------------------------------------- */

static void make_window(void) {
#ifdef __ANDROID__
    g_gles = 1;
#else
    g_gles = getenv("MHFU_GLES") != NULL;
#endif
    if (g_gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    const int w = g_out_w > 0 ? g_out_w : PSP_W * g_scale, h = g_out_h > 0 ? g_out_h : PSP_H * g_scale;
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    /* Landscape only: a resizable window otherwise lets SDL turn the phone
     * whichever way it likes, over the manifest's sensorLandscape. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (g_fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    g_window = SDL_CreateWindow("MHFU (recompiled)", w, h, flags);
    if (g_window) {
        g_ui = SDL_GetWindowDisplayScale(g_window);
        if (g_ui < 1) g_ui = 1;
        font_init();
    }
    SDL_LockMutex(g_lock);
    g_window_state = g_window ? 1 : -1;
    SDL_SignalCondition(g_cond);
    SDL_UnlockMutex(g_lock);
    if (!g_window) fprintf(stderr, "window: %s\n", SDL_GetError());
}

static void text_event(const SDL_Event *e) {
    SDL_LockMutex(g_lock);
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        const char *in = e->text.text;
        size_t len = strlen(g_tb_text);
        while (*in && utf8_len(g_tb_text) < g_tb_limit) {
            const char *q = in;
            utf8_next(&q);
            if (len + (size_t)(q - in) + 1 >= sizeof g_tb_text) break;
            memcpy(g_tb_text + len, in, (size_t)(q - in));
            len += (size_t)(q - in);
            g_tb_text[len] = 0;
            in = q;
        }
    } else if (e->type == SDL_EVENT_KEY_DOWN) {
        if (e->key.scancode == SDL_SCANCODE_BACKSPACE) {
            size_t len = strlen(g_tb_text);
            while (len && (g_tb_text[len - 1] & 0xC0) == 0x80) len--;    /* to the code point's first byte */
            if (len) len--;
            g_tb_text[len] = 0;
        } else if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) {
            SDL_SetAtomicInt(&g_tb_state, 2);
        } else if (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) {
            SDL_SetAtomicInt(&g_tb_state, 3);
        }
    }
    SDL_UnlockMutex(g_lock);
}

typedef struct { int (*fn)(int, char **); int argc; char **argv; int code; } game_args;

static int SDLCALL game_thread(void *arg) {
    game_args *g = (game_args *)arg;
    g->code = g->fn(g->argc, g->argv);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
    return g->code;
}

int mhfu_window_main(int (*game)(int, char **), int argc, char **argv) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");      /* Back opens the menu */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#ifndef __ANDROID__
    /* Desktop Linux: the menu is drawn for fingers; the mouse's clicks count as one. */
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "window: SDL: %s\n", SDL_GetError());
        return game(argc, argv);                  /* run without a window */
    }
    g_lock = SDL_CreateMutex();
    g_cond = SDL_CreateCondition();
    g_ev_window = SDL_RegisterEvents(2);
    SDL_AddEventWatch(app_events, NULL);
    g_ev_wake = g_ev_window + 1;
    SDL_SetAtomicInt(&g_stick, 0);
    SDL_SetAtomicInt(&g_touch_stick_atomic, -1);
    settings_load();
    g_show_fps = g_show_fps_init;
    mhfu_gamepad_external();

    /* The game, with a big stack: recompiled code nests deeply. */
    static game_args args;
    args.fn = game; args.argc = argc; args.argv = argv;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &args);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 16 << 20);
    SDL_Thread *t = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!t) { fprintf(stderr, "window: no game thread: %s\n", SDL_GetError()); return 1; }

    uint64_t next_stats = 0, next_title = 0;
    int moving = 0, quit = 0;
    uint64_t last_compose = 0;
    while (!quit) {
        SDL_Event e;
        if (SDL_GetAtomicInt(&g_tb_state) != 1) g_tb_started = 0;
        if (wait_event(&e, moving ? 16 : g_ui_pending ? 30 : 100)) {
            do {
                if (e.type == g_ev_window) { make_window(); continue; }
                if (e.type == g_ev_wake) {
                    const int tb = SDL_GetAtomicInt(&g_tb_state) == 1;
                    if (tb && !g_tb_started && g_window) {   /* just opened: the keyboard, even if one was "active" */
                        memset(g_held, 0, sizeof g_held);
                        keys_changed();
                        g_tb_hit_ok = 0;
                        if (SDL_TextInputActive(g_window)) SDL_StopTextInput(g_window);
                        SDL_StartTextInput(g_window);
                        g_tb_started = 1;
                    }
                    moving = 1;
                    continue;
                }
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    quit = 1;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    SDL_SetAtomicInt(&g_closed, 1);
                    if (g_on_close) g_on_close();   /* reports and exits */
                    quit = 1;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    memset(g_held, 0, sizeof g_held);
                    keys_changed();
                    touch_release_all();
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_SHOWN:
                case SDL_EVENT_WINDOW_RESTORED:
                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    SDL_SetAtomicInt(&g_remake, 1);
                    moving = 1;
                    break;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_CANCELED: {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
                    const float px = e.tfinger.x * ww, py = e.tfinger.y * wh;
                    if (e.type == SDL_EVENT_FINGER_DOWN && SDL_GetAtomicInt(&g_tb_state) == 1 && textbox_touch(px, py)) {
                        moving = 1;                     /* Send, Close, or the field (the keyboard again) */
                        break;
                    }
                    if (g_edit) {
                        edit_touch(&e, (float)ww, (float)wh);
                        moving = 1;
                        break;
                    }
                    if (g_menu_open) {
                        menu_touch(&e, px, py, (float)ww, (float)wh);
                        moving = 1;
                        break;
                    }
                    int slot = -1, free_slot = -1;
                    for (int i = 0; i < FINGERS; i++) {
                        if (g_fingers[i].elem && g_fingers[i].id == e.tfinger.fingerID) slot = i;
                        if (!g_fingers[i].elem && free_slot < 0) free_slot = i;
                    }
                    const layout L = layout_for((float)ww, (float)wh);
                    if (e.type == SDL_EVENT_FINGER_DOWN && slot < 0 && free_slot >= 0) {
                        const int el = hit(&L, px, py);
                        if (el == T_MENU) { menu_set(1); moving = 1; break; }
                        if (el == T_CHAT) { mhfu_chat_request(); moving = 1; break; }   /* the room chat */
                        if (px < ww * 0.025f) {          /* the left edge: a swipe opens the drawer */
                            g_fingers[free_slot].id = e.tfinger.fingerID;
                            g_fingers[free_slot].elem = T_EDGE;
                            g_fingers[free_slot].x = px;
                            g_fingers[free_slot].y = py;
                            break;
                        }
                        if (el == T_BAG && l_toggles() && vpad_visible()) {   /* the bag: a toggle, no finger kept */
                            g_l_held = !g_l_held;
                            touch_update((float)ww, (float)wh);
                            moving = 1;
                            break;
                        }
                        if (el && vpad_visible()) {
                            g_fingers[free_slot].id = e.tfinger.fingerID;
                            g_fingers[free_slot].elem = el;
                            g_fingers[free_slot].x = g_fingers[free_slot].sx = px;
                            g_fingers[free_slot].y = g_fingers[free_slot].sy = py;
                            g_fingers[free_slot].t0 = SDL_GetTicks();
                            g_fingers[free_slot].dir = TGT_UNDECIDED;
                        } else if (!el || !vpad_visible()) {   /* nothing there: the camera */
                            g_fingers[free_slot].id = e.tfinger.fingerID;
                            g_fingers[free_slot].elem = T_SWIPE;
                            g_fingers[free_slot].x = g_fingers[free_slot].sx = px;
                            g_fingers[free_slot].y = g_fingers[free_slot].sy = py;
                        }
                    } else if (slot >= 0 && e.type == SDL_EVENT_FINGER_MOTION && g_fingers[slot].elem == T_EDGE) {
                        if (px - g_fingers[slot].x > ww * 0.05f) { g_fingers[slot].elem = T_NONE; menu_set(1); moving = 1; }
                        break;
                    } else if (slot >= 0 && e.type == SDL_EVENT_FINGER_MOTION) {
                        finger *f = &g_fingers[slot];
                        f->x = px;
                        f->y = py;
                        /* A finger slides from button to button; the stick and
                         * the d-pad keep it wherever it goes. */
                        if (f->elem != T_STICK && f->elem != T_DPAD && f->elem != T_SWIPE && f->elem != T_ATTACK &&
                            !is_target_elem(f->elem)) {        /* the target button keeps its finger: it is slid */
                            const int el = hit(&L, px, py);
                            if (el && el != T_MENU && el != T_STICK && el != T_DPAD && el != T_RSTICK && el != T_ATTACK) f->elem = el;
                        }
                    } else if (slot >= 0) {
                        /* A target button let go before it was told: a tap, lock on. */
                        if (is_target_elem(g_fingers[slot].elem) && g_fingers[slot].dir == TGT_UNDECIDED)
                            g_target_tap_until = SDL_GetTicks() + TARGET_TAP_MS;
                        g_fingers[slot].elem = T_NONE;
                    }
                    touch_update((float)ww, (float)wh);
                    moving = 1;
                    break;
                }
                case SDL_EVENT_TEXT_INPUT:
                    if (SDL_GetAtomicInt(&g_tb_state) == 1) text_event(&e);
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    const int down = e.type == SDL_EVENT_KEY_DOWN;
                    if (SDL_GetAtomicInt(&g_tb_state) == 1) {
                        if (down) text_event(&e);
                        if (SDL_GetAtomicInt(&g_tb_state) != 1) SDL_StopTextInput(g_window);
                        break;
                    }
                    if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F3) { g_show_fps = !g_show_fps; moving = 1; break; }
                    if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1 && mhfu_chat_available()) {
                        mhfu_chat_request();                     /* in a room F1 is the chat's */
                        break;
                    }
                    if (down && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_AC_BACK || e.key.scancode == SDL_SCANCODE_F1)) {
                        if (g_edit) edit_close();
                        else menu_set(!g_menu_open);
                        moving = 1;
                        break;
                    }
                    if (down && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_F11 ||
                                                  (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        g_fullscreen = !g_fullscreen;
                        SDL_SetWindowFullscreen(g_window, g_fullscreen);
                        break;
                    }
                    if (e.key.scancode < SDL_SCANCODE_COUNT) {
                        g_held[e.key.scancode] = (unsigned char)down;
                        keys_changed();
                    }
                    break;
                }
                default:
                    /* R3 (the right stick pressed; the PSP has none) opens the menu: on the
                     * Steam Deck a controller is always there and there is no F1. */
                    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                        if (g_edit) edit_close();
                        else menu_set(!g_menu_open);
                        moving = 1;
                        break;
                    }
                    mhfu_gamepad_event(&e);
                    /* With one connected the touch controls step aside (Automatic). */
                    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) moving = 1;
                    break;
                }
            } while (!quit && SDL_PollEvent(&e));
        }
        mhfu_gamepad_update();
        if (g_window && target_pending()) {      /* the target button's hold and tap run on time */
            int ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
            touch_update((float)ww, (float)wh);
            moving = 1;                         /* wake again soon (16 ms, not 100) */
        }
        const uint64_t now = SDL_GetTicks();
        if (now >= next_stats) {
            mhfu_stats_tick(now, g_fps_text, sizeof g_fps_text);
            next_stats = now + 500;
            moving = 1;
        }
        if (g_l_held && !l_toggles() && g_window) {   /* out of the quest: let L go */
            int ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
            touch_update((float)ww, (float)wh);
            moving = 1;
        }
        if (ui_ctx_settled() != g_uictx) moving = 1;   /* a menu opened, an NPC came near: other buttons
                                                   * (settled: a flag that flickers must not redraw every frame) */
        /* At most ~30 overlays a second: each is the whole screen drawn on the
         * CPU, and a finger on the stick sends over a hundred motion events a
         * second, which kept this thread busier than the game's (lag, 2026-11-09). */
        if (moving && g_window && SDL_GetTicks() - last_compose >= COMPOSE_MS) {
            static overlay_sig drawn;
            overlay_sig sig;
            const int steady = overlay_sig_now(&sig);
            if (steady && !memcmp(&sig, &drawn, sizeof sig) && SDL_GetTicks() - last_compose < 500) moving = 0;
            else {
                moving = compose();
                last_compose = SDL_GetTicks();
                if (steady) drawn = sig;
                else memset(&drawn, 0xFF, sizeof drawn);
            }
        }
        if (g_window && now >= next_title) {
            char status[128], title[200];
            mhfu_lobby_status(status, sizeof status);
            snprintf(title, sizeof title, "MHFU (recompiled) - %s", status);
            SDL_SetWindowTitle(g_window, title);
            next_title = now + 2000;
        }
    }
    int code = 0;
    if (!SDL_GetAtomicInt(&g_closed)) SDL_WaitThread(t, &code);
    return code;
}

#endif
