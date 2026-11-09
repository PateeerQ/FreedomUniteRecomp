/* MHFU-Android -- the room's chat. MHFU has none of its own, so while this
 * player is in a multiplayer room, a key of its own (F1 by default,
 * MHFU_CHAT_KEY=<input.ini key name>) on PC, or the chat bubble beside the menu
 * button on a phone, opens a text box over the game (window.h) and the line
 * goes to everyone else in the room (net/client.h, MHN_CHAT). Lines that come in
 * are shown over the game for a while. Select stays the game's (it kicks). */
#ifndef MHFU_CHAT_H
#define MHFU_CHAT_H

#include <stdint.h>

/* On every controller read, on the game's thread: takes the buttons and
 * stick about to go to the game and returns the buttons it should see
 * (without Select in a room, none while the chat box is open). */
uint32_t mhfu_chat_buttons(uint32_t buttons, uint8_t *ax, uint8_t *ay);

/* Open the chat box at the next controller read (from any thread); nothing
 * outside a room. */
void mhfu_chat_request(void);

/* The chat key: its Windows virtual-key code (keys.h), and its name for the
 * room's hint ("F1"). */
int mhfu_chat_key_vk(void);
const char *mhfu_chat_key_label(void);

/* 1 while in a room (the phone's chat bubble shows then). */
int mhfu_chat_available(void);

#endif
