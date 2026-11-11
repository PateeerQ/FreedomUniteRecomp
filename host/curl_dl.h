/* HTTP(S) on Linux through the system's libcurl, loaded with dlopen (Windows
 * uses WinHTTP, Android Java). Used by host/achievements.c and the launcher. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when libcurl.so.4 could be opened. */
int mhfu_curl_available(void);

/* GET, or POST when `post` is given (post_len bytes, `ctype` or a form). The
 * body is malloc'd and NUL-terminated (free it); returns the HTTP status, or 0
 * when nothing came back (`err` then says why). */
int mhfu_curl_request(const char *url, const char *post, size_t post_len, const char *ctype, const char *accept,
                      const char *agent, char **body, size_t *len, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
