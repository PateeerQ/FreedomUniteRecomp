/* HTTP(S) on Linux through the system's libcurl, opened at run time (see
 * curl_dl.h). Every desktop Linux and SteamOS has libcurl.so.4; opening it with
 * dlopen keeps the build free of its headers and the game free of a hard
 * dependency (without it, online features say so and the game still runs). */
#if !defined(_WIN32) && !defined(__ANDROID__)
#include "curl_dl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libcurl's ABI (stable since 7.x): option numbers and the few calls used. */
enum {
    OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018,
    OPT_HTTPHEADER = 10023, OPT_WRITEFUNCTION = 20011, OPT_TIMEOUT = 13, OPT_FOLLOWLOCATION = 52,
    OPT_POSTFIELDSIZE = 60, OPT_CONNECTTIMEOUT = 78, OPT_NOSIGNAL = 99,
    INFO_RESPONSE_CODE = 0x200002,
};
typedef void CURL;
struct slist;
static CURL *(*p_init)(void);
static int (*p_setopt)(CURL *, int, ...);
static int (*p_perform)(CURL *);
static int (*p_getinfo)(CURL *, int, ...);
static void (*p_cleanup)(CURL *);
static struct slist *(*p_slist_append)(struct slist *, const char *);
static void (*p_slist_free_all)(struct slist *);
static const char *(*p_strerror)(int);
static int (*p_global_init)(long);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;

static void load(void) {
    static const char *names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
#define SYM(v, n) if (!(*(void **)&v = dlsym(h, n))) return
    SYM(p_init, "curl_easy_init");
    SYM(p_setopt, "curl_easy_setopt");
    SYM(p_perform, "curl_easy_perform");
    SYM(p_getinfo, "curl_easy_getinfo");
    SYM(p_cleanup, "curl_easy_cleanup");
    SYM(p_slist_append, "curl_slist_append");
    SYM(p_slist_free_all, "curl_slist_free_all");
    SYM(p_strerror, "curl_easy_strerror");
    SYM(p_global_init, "curl_global_init");
#undef SYM
    p_global_init(3 /* CURL_GLOBAL_ALL */);
    g_ok = 1;
}

int mhfu_curl_available(void) {
    pthread_once(&g_once, load);
    return g_ok;
}

typedef struct { char *p; size_t n; } buf;

static size_t on_data(char *d, size_t size, size_t count, void *u) {
    buf *b = (buf *)u;
    const size_t n = size * count;
    char *nb = (char *)realloc(b->p, b->n + n + 1);
    if (!nb) return 0;
    b->p = nb;
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
    return n;
}

int mhfu_curl_request(const char *url, const char *post, size_t post_len, const char *ctype, const char *accept,
                      const char *agent, char **body, size_t *len, char *err, size_t err_size) {
    *body = NULL;
    *len = 0;
    if (err && err_size) err[0] = 0;
    if (!mhfu_curl_available()) {
        if (err && err_size) strncpy(err, "libcurl not found (install curl)", err_size - 1);
        return 0;
    }
    CURL *c = p_init();
    if (!c) return 0;
    buf b = {NULL, 0};
    struct slist *hd = NULL;
    char line[256];
    if (accept) { snprintf(line, sizeof line, "Accept: %s", accept); hd = p_slist_append(hd, line); }
    if (post) {
        snprintf(line, sizeof line, "Content-Type: %s", ctype ? ctype : "application/x-www-form-urlencoded");
        hd = p_slist_append(hd, line);
    }
    p_setopt(c, OPT_URL, url);
    p_setopt(c, OPT_NOSIGNAL, 1L);
    p_setopt(c, OPT_FOLLOWLOCATION, 1L);
    p_setopt(c, OPT_CONNECTTIMEOUT, 10L);
    p_setopt(c, OPT_TIMEOUT, 60L);
    if (agent) p_setopt(c, OPT_USERAGENT, agent);
    if (hd) p_setopt(c, OPT_HTTPHEADER, hd);
    if (post) {
        p_setopt(c, OPT_POSTFIELDSIZE, (long)post_len);
        p_setopt(c, OPT_POSTFIELDS, post);
    }
    p_setopt(c, OPT_WRITEFUNCTION, on_data);
    p_setopt(c, OPT_WRITEDATA, &b);
    long status = 0;
    const int rc = p_perform(c);
    if (rc == 0) p_getinfo(c, INFO_RESPONSE_CODE, &status);
    else if (err && err_size) snprintf(err, err_size, "no connection (%s)", p_strerror(rc));
    if (hd) p_slist_free_all(hd);
    p_cleanup(c);
    *body = b.p;
    *len = b.n;
    return (int)status;
}
#endif
