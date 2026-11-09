/* MHFU-Android -- a small sampling profiler, for finding where the time goes
 * (MHFU_PROFILE=1). Windows only for now: a thread wakes about every
 * millisecond, suspends each of the process's other threads in turn, reads
 * where it is (RIP) and lets it go. The report names the busiest functions per
 * thread (DbgHelp over the exe's PDB: the link writes one, see CMakeLists.txt)
 * and each thread's CPU time against the wall clock, which says which thread
 * holds the frame up. Android: use the NDK's simpleperf instead. */
#include "prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#define MAX_THREADS 64
#define TAB_BITS    16

typedef struct { DWORD64 pc; unsigned n; } slot;
typedef struct {
    DWORD id;
    HANDLE h;
    unsigned samples;
    slot tab[1 << TAB_BITS];       /* open addressing by pc */
    ULONGLONG cpu0;                /* user+kernel 100 ns at the start */
} pthread_rec;

static pthread_rec *g_th[MAX_THREADS];
static int g_nth;
static volatile LONG g_run;
static ULONGLONG g_t0;
static DWORD g_self;

static ULONGLONG cpu_of(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static void refresh_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof te };
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_self) continue;
        int known = 0;
        for (int i = 0; i < g_nth; i++) if (g_th[i]->id == te.th32ThreadID) known = 1;
        if (known || g_nth == MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        pthread_rec *t = (pthread_rec *)calloc(1, sizeof *t);
        if (!t) { CloseHandle(h); continue; }
        t->id = te.th32ThreadID;
        t->h = h;
        t->cpu0 = cpu_of(h);
        g_th[g_nth++] = t;
    }
    CloseHandle(snap);
}

static void add(pthread_rec *t, DWORD64 pc) {
    unsigned i = (unsigned)((pc * 0x9E3779B97F4A7C15ull) >> (64 - TAB_BITS));
    for (unsigned n = 0; n < (1u << TAB_BITS); n++, i = (i + 1) & ((1u << TAB_BITS) - 1)) {
        if (t->tab[i].pc == pc) { t->tab[i].n++; break; }
        if (!t->tab[i].pc) { t->tab[i].pc = pc; t->tab[i].n = 1; break; }
    }
    t->samples++;
}

static DWORD WINAPI sampler(LPVOID arg) {
    /* MHFU_PROFILE=N (N > 1): start after N seconds (past the loading). */
    const int delay = (int)(intptr_t)arg;
    if (delay > 1) Sleep((DWORD)delay * 1000);
    g_t0 = GetTickCount64();
    timeBeginPeriod(1);
    unsigned tick = 0;
    while (g_run) {
        if ((tick++ & 1023) == 0) refresh_threads();
        for (int i = 0; i < g_nth; i++) {
            pthread_rec *t = g_th[i];
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t->h, &ctx)) add(t, ctx.Rip);
            ResumeThread(t->h);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
}

void mhfu_prof_start(void) {
    const char *e = getenv("MHFU_PROFILE");
    if (!e || !*e || !strcmp(e, "0")) return;
    g_t0 = GetTickCount64();
    g_run = 1;
    const int delay = atoi(e);
    HANDLE h = CreateThread(NULL, 0, sampler, (LPVOID)(intptr_t)delay, 0, &g_self);
    if (h) CloseHandle(h);
    printf("profile: sampling every thread about every millisecond%s\n", delay > 1 ? ", after a delay" : "");
}

typedef struct { char name[160]; unsigned n; } fn_count;
static int by_n(const void *a, const void *b) {
    const fn_count *x = (const fn_count *)a, *y = (const fn_count *)b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

void mhfu_prof_report(void) {
    if (!g_run) return;
    g_run = 0;
    Sleep(20);
    const double wall = (double)(GetTickCount64() - g_t0) / 1000.0;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const int sym = SymInitialize(proc, NULL, TRUE);
    printf("profile: %.1f s of wall time, %d threads watched\n", wall, g_nth);
    for (int i = 0; i < g_nth; i++) {
        pthread_rec *t = g_th[i];
        const double cpu = (double)(cpu_of(t->h) - t->cpu0) / 1e7;
        if (t->samples < 50 && cpu < 0.5) continue;
        /* the thread's name, if it has one */
        char tname[64] = "";
        typedef HRESULT (WINAPI *gtd_fn)(HANDLE, PWSTR *);
        gtd_fn gtd = (gtd_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
        PWSTR w = NULL;
        if (gtd && SUCCEEDED(gtd(t->h, &w)) && w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, tname, sizeof tname, NULL, NULL); LocalFree(w); }
        printf("profile: thread %lu %s: CPU %.1f s (%.0f%% of one core), %u samples\n", (unsigned long)t->id, tname,
               cpu, wall > 0 ? cpu * 100 / wall : 0, t->samples);
        /* by function */
        fn_count *fc = (fn_count *)calloc(4096, sizeof *fc);
        int nf = 0;
        /* By kind: from the function's name, else from its source file. */
        static const char *const kinds[] = { "waiting", "guest code", "memory access", "trace/checks", "GE (ge.c)",
                                             "renderer (render_gl.c)", "VFPU", "threads/dispatch", "host", "other" };
        unsigned kn[10] = { 0 };
        for (unsigned k = 0; k < (1u << TAB_BITS); k++) {
            if (!t->tab[k].pc) continue;
            char name[160] = "?";
            int kind = 9;
            if (sym) {
                char buf[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
                memset(buf, 0, sizeof buf);
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(proc, t->tab[k].pc, &disp, si)) snprintf(name, sizeof name, "%s", si->Name);
                else {
                    IMAGEHLP_MODULE64 mi = { sizeof mi };
                    if (SymGetModuleInfo64(proc, t->tab[k].pc, &mi)) snprintf(name, sizeof name, "[%s]", mi.ModuleName);
                }
                const char *n = name;
                if (strstr(n, "Wait") || strstr(n, "Sleep") || strstr(n, "Alert") || strstr(n, "GetMessage")) kind = 0;
                else if (!strncmp(n, "psp_func_", 9) || !strncmp(n, "psp_at_", 7) || !strncmp(n, "psp_import_", 11)) kind = 1;
                else if (!strncmp(n, "psp_read", 8) || !strncmp(n, "psp_write", 9) || !strcmp(n, "psp_mem_ptr") ||
                         !strncmp(n, "note_", 5) || !strcmp(n, "bad_access")) kind = 2;
                else if (!strncmp(n, "psp_trace", 9) || !strncmp(n, "sp_", 3) || !strcmp(n, "psp_frame_overflow")) kind = 3;
                else {
                    IMAGEHLP_LINE64 li = { sizeof li };
                    DWORD d32 = 0;
                    if (SymGetLineFromAddr64(proc, t->tab[k].pc, &d32, &li) && li.FileName) {
                        const char *f = li.FileName;
                        if (strstr(f, "\\ge.c") || strstr(f, "/ge.c")) kind = 4;
                        else if (strstr(f, "render_gl")) kind = 5;
                        else if (strstr(f, "vfpu")) kind = 6;
                        else if (strstr(f, "threadman") || strstr(f, "dispatch") || strstr(f, "cpu.c")) kind = 7;
                        else if (strstr(f, "\\host\\") || strstr(f, "/host/")) kind = 8;
                    }
                }
            }
            kn[kind] += t->tab[k].n;
            int j = 0;
            for (; j < nf; j++) if (!strcmp(fc[j].name, name)) break;
            if (j == nf) {
                if (nf == 4096) continue;
                snprintf(fc[nf++].name, sizeof fc[0].name, "%s", name);
            }
            fc[j].n += t->tab[k].n;
        }
        qsort(fc, (size_t)nf, sizeof *fc, by_n);
        printf("profile:   by kind:");
        for (int k = 0; k < 10; k++) if (kn[k]) printf("  %s %.1f%%", kinds[k], kn[k] * 100.0 / t->samples);
        printf("\n");
        for (int j = 0; j < nf && j < 25; j++)
            printf("profile:   %5.1f%%  %s\n", fc[j].n * 100.0 / t->samples, fc[j].name);
        free(fc);
    }
    if (sym) SymCleanup(proc);
    fflush(stdout);
}

#else
void mhfu_prof_start(void) { }
void mhfu_prof_report(void) { }
#endif
