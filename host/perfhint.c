/* Android performance hints (ADPF, APerformanceHint in libandroid, API 33+).
 *
 * The session holds the threads a frame's work runs on (the game thread that
 * reads the controller, the render thread) and gets each game frame's length
 * against the target (1/30 or 1/60 s). A frame on time keeps the clocks where
 * they are; a late one makes the system raise them at once, where the default
 * governor ramps up only after the load has been high for a while: the lag
 * low-end phones show in busy scenes.
 *
 * The reported length is the whole interval between two game frames, waits
 * included: when the game keeps up it equals the target, so the system holds,
 * never lowers below what the game needs. Intervals over 250 ms (loading, the
 * app in the background) are not reported.
 *
 * Loaded with dlopen: minSdk is 26 and the calls exist from 33 (setThreads
 * from 34; before that the session is made again when a thread joins). */
#include "perfhint.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

static APerformanceHintManager *(*p_get_manager)(void);
static APerformanceHintSession *(*p_create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
static int (*p_update_target)(APerformanceHintSession *, int64_t);
static int (*p_report)(APerformanceHintSession *, int64_t);
static void (*p_close)(APerformanceHintSession *);
static int (*p_set_threads)(APerformanceHintSession *, const pid_t *, size_t);

#define MAX_TIDS 8
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_loaded;                  /* 0 not tried, 1 ready, -1 not available */
static APerformanceHintManager *g_mgr;
static APerformanceHintSession *g_sess;
static int32_t g_tids[MAX_TIDS];
static int g_ntids;
static int g_dirty;                   /* threads changed since the session was made */
static int64_t g_target;
static int64_t g_last;

static int load(void) {
    if (g_loaded) return g_loaded > 0;
    g_loaded = -1;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return 0;
    p_get_manager = (APerformanceHintManager *(*)(void))dlsym(lib, "APerformanceHint_getManager");
    p_create = (APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))
        dlsym(lib, "APerformanceHint_createSession");
    p_update_target = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    p_report = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    p_close = (void (*)(APerformanceHintSession *))dlsym(lib, "APerformanceHint_closeSession");
    p_set_threads = (int (*)(APerformanceHintSession *, const pid_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
    if (!p_get_manager || !p_create || !p_update_target || !p_report || !p_close) {
        printf("perfhint: not available on this Android (needs 13)\n");
        return 0;
    }
    g_mgr = p_get_manager();
    if (!g_mgr) { printf("perfhint: no hint manager\n"); return 0; }
    g_loaded = 1;
    return 1;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

void mhfu_perfhint_thread(void) {
    static __thread int t_done;
    if (t_done) return;
    t_done = 1;
    pthread_mutex_lock(&g_lock);
    if (load() && g_ntids < MAX_TIDS) {
        g_tids[g_ntids++] = (int32_t)gettid();
        g_dirty = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void mhfu_perfhint_frame(int fps) {
    mhfu_perfhint_thread();
    int64_t t = now_ns();
    pthread_mutex_lock(&g_lock);
    if (g_loaded > 0) {
        int64_t target = 1000000000ll / (fps > 0 ? fps : 30);
        if (g_dirty) {
            if (g_sess && p_set_threads && p_set_threads(g_sess, (const pid_t *)g_tids, (size_t)g_ntids) == 0) {
                printf("perfhint: %d threads in the session\n", g_ntids);
            } else {
                if (g_sess) p_close(g_sess);
                g_sess = p_create(g_mgr, g_tids, (size_t)g_ntids, target);
                g_target = target;
                printf("perfhint: session %s for %d threads, target %.1f ms\n", g_sess ? "made" : "refused",
                       g_ntids, target / 1e6);
                if (!g_sess) g_loaded = -1;
            }
            g_dirty = 0;
        }
        if (g_sess) {
            if (target != g_target) { p_update_target(g_sess, target); g_target = target; }
            int64_t dt = g_last ? t - g_last : 0;
            if (dt > 0 && dt < 250000000ll) p_report(g_sess, dt);
        }
    }
    g_last = t;
    pthread_mutex_unlock(&g_lock);
}

#else
void mhfu_perfhint_thread(void) {}
void mhfu_perfhint_frame(int fps) { (void)fps; }
#endif
