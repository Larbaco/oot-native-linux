// A sampling profiler for performance work (M6, docs/m6-input-gameplay.md). DM_CTL "prof <seconds>" (hle_hid.c) or NXRT_PROF=<start s>,<seconds>: the threads
// that used more than 5 % of a core over the first half second are sampled 1,000 times a second (SuspendThread, GetThreadContext's Rip, ResumeThread; nothing is
// allocated or logged while a thread is stopped), then the log gets, per thread, where its time went: by module, and by function. Inside the game DLL the function
// is the guest function the host code belongs to (nxrt_host_to_guest, "main+0x..."), inside nxrt the host function from nxrt.pdb when it is beside the
// executable (DbgHelp; NXRT_PDB=ON builds one), elsewhere the module. Written from Microsoft's documentation of the Toolhelp, thread and DbgHelp APIs.
#include "dm.h"
#include <tlhelp32.h>
#include <dbghelp.h>

enum { MAXT = 12, MAXS = 200000 };
typedef struct { DWORD tid; HANDLE h; char name[64]; uint64_t cpu0; uint32_t n; } pthr;
typedef struct { uint64_t rip; uint16_t t; } psample;
uint64_t nxrt_host_to_guest(uint64_t host, uint64_t *off);

static uint64_t thread_cpu_100ns(HANDLE h) { FILETIME c, e, k, u; if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0; return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime); }
static void thread_name(HANDLE h, char *out, size_t n) {
    PWSTR w = NULL; out[0] = 0;
    if (SUCCEEDED(GetThreadDescription(h, &w)) && w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL); LocalFree(w); }
    if (!out[0]) snprintf(out, n, "thread %lu", (unsigned long)GetThreadId(h));
}
// Where an address is: "main+0x1234" (a guest function), "nxrt!fn+0x12", or the module's name
static void where(uint64_t rip, char *out, size_t n, int fine) {
    uint64_t off = 0, g = nxrt_host_to_guest(rip, &off);
    if (g) {
        module_t *m = module_of((void *)(uintptr_t)g);
        if (m) snprintf(out, n, "game %s+%#llx", m->name, (unsigned long long)(g - (uint64_t)(uintptr_t)m->base)); else snprintf(out, n, "game %#llx", (unsigned long long)g);
        return;
    }
    HMODULE mod = NULL; char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &mod) && mod) GetModuleFileNameA(mod, path, MAX_PATH);
    const char *base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    if (fine) {
        char sb[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)sb; memset(sb, 0, sizeof sb); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; DWORD64 d = 0;
        if (SymFromAddr(GetCurrentProcess(), rip, &d, si)) { snprintf(out, n, "%s!%s", base, si->Name); return; }
    }
    snprintf(out, n, "%s", mod ? base : "?");
}
typedef struct { char key[128]; uint32_t n; } bucket;
static int by_count(const void *a, const void *b) { uint32_t x = ((const bucket *)a)->n, y = ((const bucket *)b)->n; return (x < y) - (x > y); }
static void tally(bucket *b, int *nb, int cap, const char *key) {
    for (int i = 0; i < *nb; i++) if (!strcmp(b[i].key, key)) { b[i].n++; return; }
    if (*nb < cap) { snprintf(b[*nb].key, sizeof b[0].key, "%s", key); b[(*nb)++].n = 1; }
}

static void *prof_main(void *arg) {
    double secs = *(double *)arg; free(arg);
    plat_thread_name("profiler");
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    // the busy threads: CPU over half a second
    enum { MAXALL = 512 }; static pthr all[MAXALL]; int na = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = {sizeof te};
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || na >= MAXALL) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) { all[na].tid = te.th32ThreadID; all[na].h = h; all[na].cpu0 = thread_cpu_100ns(h); na++; }
    } while (Thread32Next(snap, &te));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    Sleep(500);
    pthr t[MAXT]; int nt = 0;
    for (;;) {   // the busiest first
        int best = -1; uint64_t bu = 0;
        for (int i = 0; i < na; i++) { if (!all[i].h) continue; uint64_t u = thread_cpu_100ns(all[i].h) - all[i].cpu0; if (u > bu) { bu = u; best = i; } }
        if (best < 0 || nt == MAXT || bu < 250000) break;   // 5 % of 0.5 s = 25 ms
        t[nt] = all[best]; t[nt].n = 0; thread_name(t[nt].h, t[nt].name, sizeof t[nt].name); t[nt].cpu0 = thread_cpu_100ns(t[nt].h); all[best].h = NULL; nt++;
    }
    for (int i = 0; i < na; i++) if (all[i].h) CloseHandle(all[i].h);
    psample *s = malloc(sizeof *s * MAXS); uint32_t ns = 0;
    if (!s || !nt) { LOG("profiler: %s", nt ? "no memory" : "no thread was busy"); free(s); return NULL; }
    LOG("profiler: sampling the %d busiest threads for %.1f s at 1 kHz", nt, secs);
    uint64_t t0 = now_ns(), end = t0 + (uint64_t)(secs * 1e9);
    while (now_ns() < end && ns + MAXT < MAXS) {
        for (int i = 0; i < nt; i++) {
            if (SuspendThread(t[i].h) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t[i].h, &c)) { s[ns].rip = c.Rip; s[ns].t = (uint16_t)i; ns++; t[i].n++; }
            ResumeThread(t[i].h);
        }
        plat_sleep_ns(1000000);
    }
    double wall = (now_ns() - t0) / 1e9;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); int sym = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    enum { NB = 4096 }; bucket *fb = calloc(NB, sizeof *fb), *mb = calloc(64, sizeof *mb);
    for (int i = 0; i < nt; i++) {
        int nf = 0, nm = 0; char w[160];
        for (uint32_t k = 0; k < ns; k++) if (s[k].t == i) {
            where(s[k].rip, w, sizeof w, sym); tally(fb, &nf, NB, w);
            char m[64]; snprintf(m, sizeof m, "%s", w); char *sp = strchr(m, ' '); char *bang = strchr(m, '!'); if (sp && !strncmp(m, "game", 4)) *sp = 0; if (bang) *bang = 0; tally(mb, &nm, 64, m);
        }
        qsort(fb, (size_t)nf, sizeof *fb, by_count); qsort(mb, (size_t)nm, sizeof *mb, by_count);
        double cpu = (thread_cpu_100ns(t[i].h) - t[i].cpu0) / 1e7 / wall;
        char line[600]; int o = snprintf(line, sizeof line, "profiler: '%s' %.0f %% of a core, %u samples:", t[i].name, 100 * cpu, t[i].n);
        for (int k = 0; k < nm && o < 560; k++) o += snprintf(line + o, sizeof line - o, " %s %.1f %%", mb[k].key, 100.0 * mb[k].n / (t[i].n ? t[i].n : 1));
        LOG("%s", line);
        for (int k = 0; k < nf && k < 25; k++) LOG("profiler:   %5.1f %%  %s", 100.0 * fb[k].n / (t[i].n ? t[i].n : 1), fb[k].key);
        memset(fb, 0, NB * sizeof *fb); memset(mb, 0, 64 * sizeof *mb);
        CloseHandle(t[i].h);
    }
    if (sym) SymCleanup(GetCurrentProcess());
    free(fb); free(mb); free(s);
    return NULL;
}
void nxrt_prof_start(double secs) {
    if (secs <= 0) secs = 5;
    double *a = malloc(sizeof *a); *a = secs;
    pthread_t th; if (!pthread_create(&th, NULL, prof_main, a)) pthread_detach(th);
}
static void *prof_delayed(void *arg) { double *a = arg; Sleep((DWORD)(a[0] * 1000)); nxrt_prof_start(a[1]); free(a); return NULL; }
void nxrt_prof_env(void) {   // NXRT_PROF=<start s>,<seconds>
    const char *e = getenv("NXRT_PROF"); double st, sec;
    if (!e || sscanf(e, "%lf,%lf", &st, &sec) != 2) return;
    double *a = malloc(2 * sizeof *a); a[0] = st; a[1] = sec;
    pthread_t th; if (!pthread_create(&th, NULL, prof_delayed, a)) pthread_detach(th);
}
