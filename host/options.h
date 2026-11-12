/* MHFU-Android -- options the game changes while it runs (volume, the mouse
 * camera's speed) kept in the launcher's settings, so the next start has them. */
#ifndef MHFU_OPTIONS_H
#define MHFU_OPTIONS_H

/* `key=value` replaced or added in MHFU_LAUNCHER_INI (set by the launcher);
 * nothing when the game was started without the launcher. */
void mhfu_option_save(const char *key, int value);

#endif
