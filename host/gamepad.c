/* MHFU-Android -- controllers through SDL3. See gamepad.h.
 *
 * SDL is initialised, polled and read on one thread of its own, as it wants;
 * the game thread only reads the last state from three words. */
#include "gamepad.h"
#include "input.h"      /* PSP_CTRL_* */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_AtomicInt g_buttons;
static SDL_AtomicInt g_stick;        /* x | y << 8, or -1 while the stick is centred */

/* Past this, the right stick presses the D-pad. */
#define RIGHT_STICK_PRESS 16000
/* Within this the left stick is centred, and the keyboard's stick is used. */
#define LEFT_STICK_DEAD   6000

static int held(SDL_Gamepad *pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b) ? 1 : 0; }

static uint32_t read_buttons(SDL_Gamepad *pad) {
    uint32_t b = 0;
    if (held(pad, SDL_GAMEPAD_BUTTON_SOUTH))          b |= PSP_CTRL_CROSS;
    if (held(pad, SDL_GAMEPAD_BUTTON_EAST))           b |= PSP_CTRL_CIRCLE;
    if (held(pad, SDL_GAMEPAD_BUTTON_WEST))           b |= PSP_CTRL_SQUARE;
    if (held(pad, SDL_GAMEPAD_BUTTON_NORTH))          b |= PSP_CTRL_TRIANGLE;
    if (held(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= PSP_CTRL_LTRIGGER;
    if (held(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= PSP_CTRL_RTRIGGER;
    if (held(pad, SDL_GAMEPAD_BUTTON_START))          b |= PSP_CTRL_START;
    if (held(pad, SDL_GAMEPAD_BUTTON_BACK))           b |= PSP_CTRL_SELECT;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))        b |= PSP_CTRL_UP;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      b |= PSP_CTRL_DOWN;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      b |= PSP_CTRL_LEFT;
    if (held(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     b |= PSP_CTRL_RIGHT;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)  b |= PSP_CTRL_LTRIGGER;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= PSP_CTRL_RTRIGGER;

    const int rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    const int ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    if (rx < -RIGHT_STICK_PRESS) b |= PSP_CTRL_LEFT;
    if (rx >  RIGHT_STICK_PRESS) b |= PSP_CTRL_RIGHT;
    if (ry < -RIGHT_STICK_PRESS) b |= PSP_CTRL_UP;
    if (ry >  RIGHT_STICK_PRESS) b |= PSP_CTRL_DOWN;
    return b;
}

static int read_stick(SDL_Gamepad *pad) {
    const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x > -LEFT_STICK_DEAD && x < LEFT_STICK_DEAD && y > -LEFT_STICK_DEAD && y < LEFT_STICK_DEAD) return -1;
    return ((x + 32768) >> 8) | ((y + 32768) >> 8) << 8;
}

/* The controllers in use. Every one connected is read and their buttons
 * combined: Android handhelds (AYN Thor, Odin, Retroid) show more than one
 * "gamepad" to SDL (a fingerprint sensor or a remote that sends d-pad keys
 * counts as one too), and taking only the first one connected left the
 * built-in controller unread. */
#define MAX_PADS 8
static SDL_Gamepad *g_pads[MAX_PADS];
static int g_external;               /* the SDL window's loop feeds the events */

/* A real controller has face buttons and a stick; a d-pad-only device (a
 * fingerprint sensor, a TV remote) is read but does not hide the touch
 * controls. */
static int pad_is_full(SDL_Gamepad *p) {
    return SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_SOUTH) && SDL_GamepadHasAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
}

void mhfu_gamepad_event(const SDL_Event *e) {
    /* MHFU_GAMEPAD_GUID (the launcher's Controller page): only that device;
     * otherwise every one connected. */
    static const char *want;
    static int got_want;
    if (!got_want) { want = getenv("MHFU_GAMEPAD_GUID"); got_want = 1; }
    if (getenv("MHFU_NO_GAMEPAD")) return;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        const SDL_JoystickID id = e->gdevice.which;
        char guid[64];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
        const char *name = SDL_GetGamepadNameForID(id);
        if (!name) name = "?";
        int free_slot = -1;
        for (int i = 0; i < MAX_PADS; i++) {
            if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) return;   /* already open */
            if (!g_pads[i] && free_slot < 0) free_slot = i;
        }
        if (want && *want && SDL_strcasecmp(guid, want) != 0) {
            printf("gamepad: %s (%04x:%04x, %s) ignored: not the chosen one\n", name,
                   SDL_GetGamepadVendorForID(id), SDL_GetGamepadProductForID(id), guid);
            fflush(stdout);
            return;
        }
        if (free_slot < 0) return;
        SDL_Gamepad *p = SDL_OpenGamepad(id);
        if (!p) {
            printf("gamepad: %s could not be opened: %s\n", name, SDL_GetError());
            fflush(stdout);
            return;
        }
        g_pads[free_slot] = p;
        printf("gamepad: %s connected (%04x:%04x, %s)%s\n", name,
               SDL_GetGamepadVendor(p), SDL_GetGamepadProduct(p), guid,
               pad_is_full(p) ? "" : ", d-pad only");
        fflush(stdout);
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (int i = 0; i < MAX_PADS; i++) {
            if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e->gdevice.which) {
                printf("gamepad: %s removed\n", SDL_GetGamepadName(g_pads[i]));
                fflush(stdout);
                SDL_CloseGamepad(g_pads[i]);
                g_pads[i] = NULL;
            }
        }
    }
}

void mhfu_gamepad_update(void) {
    uint32_t b = 0;
    int stick = -1;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!g_pads[i]) continue;
        b |= read_buttons(g_pads[i]);
        if (stick < 0) stick = read_stick(g_pads[i]);     /* the first stick pushed */
    }
    SDL_SetAtomicInt(&g_buttons, (int)b);
    SDL_SetAtomicInt(&g_stick, stick);
}

void mhfu_gamepad_external(void) { g_external = 1; }
int mhfu_gamepad_connected(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i] && pad_is_full(g_pads[i])) return 1;
    return 0;
}

static int SDLCALL pad_thread(void *arg) {
    (void)arg;
    /* The window is not SDL's, so to SDL the program is always in the
     * background: without this it would ignore the controller. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepad: SDL: %s\n", SDL_GetError());
        return 0;
    }
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) mhfu_gamepad_event(&e);
        mhfu_gamepad_update();
        SDL_Delay(4);
    }
    return 0;
}

int mhfu_gamepad_start(void) {
    SDL_SetAtomicInt(&g_stick, -1);
    /* MHFU_NO_GAMEPAD=1: scripted runs, which a controller lying on the desk
     * should not steer. */
    if (getenv("MHFU_NO_GAMEPAD") || g_external) return 0;
    SDL_Thread *t = SDL_CreateThread(pad_thread, "gamepad", NULL);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}

uint32_t mhfu_gamepad_read(uint8_t *ax, uint8_t *ay) {
    const int s = SDL_GetAtomicInt(&g_stick);
    if (s >= 0) {
        *ax = (uint8_t)(s & 0xFF);
        *ay = (uint8_t)(s >> 8);
    }
    return (uint32_t)SDL_GetAtomicInt(&g_buttons);
}
