/* MHFU-Android -- see options.h. */
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mhfu_option_save(const char *key, int value) {
    const char *path = getenv("MHFU_LAUNCHER_INI");
    if (!path || !*path) return;
    static char text[64 * 1024], out[64 * 1024 + 64];
    size_t n = 0, w = 0;
    FILE *f = fopen(path, "rb");
    if (f) { n = fread(text, 1, sizeof text - 1, f); fclose(f); }
    text[n] = 0;
    const size_t kl = strlen(key);
    int done = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            w += (size_t)snprintf(out + w, sizeof out - w, "%s=%d\n", key, value);
            done = 1;
        } else {
            memcpy(out + w, line, len);
            w += len;
        }
        line += len;
    }
    if (!done) w += (size_t)snprintf(out + w, sizeof out - w, "%s%s=%d\n", w && out[w - 1] != '\n' ? "\n" : "", key, value);
    f = fopen(path, "wb");
    if (f) { fwrite(out, 1, w, f); fclose(f); }
}
