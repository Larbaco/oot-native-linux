/* Win32 implementation of plat.h. */
#include "plat.h"

#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static uint64_t qpc_freq(void) {
    static uint64_t f;
    if (!f) { LARGE_INTEGER li; QueryPerformanceFrequency(&li); f = (uint64_t)li.QuadPart; }
    return f;
}
uint64_t plat_qpc(void) { LARGE_INTEGER li; QueryPerformanceCounter(&li); return (uint64_t)li.QuadPart; }
uint64_t plat_qpc_freq(void) { return qpc_freq(); }
uint64_t plat_now_ns(void) {
    uint64_t t = plat_qpc(), f = qpc_freq();
    return t / f * 1000000000u + t % f * 1000000000u / f;   /* no 128-bit division: compiler-rt is not linked on this target */
}
uint64_t plat_realtime_ns(void) {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   /* 100 ns since 1601 */
    return (v - 116444736000000000ull) * 100u;
}

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define NXRT_THROTTLE_TIMER 0x4   /* PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION (Windows 11 SDK) */
#else
#define NXRT_THROTTLE_TIMER PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#endif
static _Thread_local HANDLE t_timer;
void plat_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    if (!t_timer) {
        t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_timer) t_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);   /* before Windows 10 1803 */
    }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);   /* relative, 100 ns units */
    if (t_timer && SetWaitableTimer(t_timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(t_timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}
void plat_yield(void) { SwitchToThread(); }

void plat_timer_resolution(void) {
    typedef UINT (WINAPI *tbp_t)(UINT);
    HMODULE w = LoadLibraryW(L"winmm.dll");
    tbp_t tbp = w ? (tbp_t)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
    if (tbp) tbp(1);
    /* Windows 11 runs a process whose window is behind others or hidden at a lower quality of service (EcoQoS: efficiency cores, lower clocks) and stops honouring its
       timer resolution: a game paced at 60 Hz opts out of both (M6 phase B: 40 presents/s with the window behind others, 59-60 with this) */
    PROCESS_POWER_THROTTLING_STATE ps = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED | NXRT_THROTTLE_TIMER, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ps, sizeof ps);
}

int plat_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    if (!addr || !ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) || got != n) return -1;
    return 0;
}

typedef struct { plat_thread_fn fn; void *arg; } start_t;
static unsigned __stdcall thread_tramp(void *p) {
    start_t s = *(start_t *)p; HeapFree(GetProcessHeap(), 0, p);
    ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee);   /* room for the crash report on a stack overflow */
    return s.fn(s.arg);
}
uint32_t plat_thread_start(plat_thread_fn fn, void *arg, size_t stack_reserve) {
    start_t *s = HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (!s) return 0;
    s->fn = fn; s->arg = arg;
    unsigned tid = 0;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_reserve, thread_tramp, s, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h) { HeapFree(GetProcessHeap(), 0, s); return 0; }
    CloseHandle((HANDLE)h);
    return tid;
}
uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }
void plat_thread_name(const char *name) {
    wchar_t w[128]; int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 128);
    if (n > 0) SetThreadDescription(GetCurrentThread(), w);
}
/* Horizon priorities run 0 (highest) to 63; this game uses 28..59 (its npdm). Windows: only a nudge, so a busy PC never starves the game's
   own lower-priority workers (research/01 lesson 11) and the game never starves the rest of the PC */
int plat_priority_for(int p) {
    if (p <= 27) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (p <= 44) return THREAD_PRIORITY_NORMAL;
    if (p <= 59) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}
void plat_thread_set_priority(uint32_t tid, int horizon_prio) {
    HANDLE h = tid == GetCurrentThreadId() ? GetCurrentThread() : OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return;
    SetThreadPriority(h, plat_priority_for(horizon_prio));
    if (h != GetCurrentThread()) CloseHandle(h);
}
double plat_thread_cpu_seconds(uint32_t tid) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return -1;
    FILETIME c, e, k, u; double s = -1;
    if (GetThreadTimes(h, &c, &e, &k, &u))
        s = (double)((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
    CloseHandle(h);
    return s;
}
_Noreturn void plat_thread_exit(void) { _endthreadex(0); for (;;) ExitThread(0); }

void plat_cpu_brand(char *out, size_t n) {
    int r[4]; char b[49] = {0};
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) { __cpuid(r, (int)(0x80000002u + (unsigned)i)); memcpy(b + 16 * i, r, 16); }
    }
    const char *s = b; while (*s == ' ') s++;
    snprintf(out, n, "%s", *s ? s : "unknown x86-64 CPU");
}
int plat_cpu_x86_64_v3(void) {
    int r[4];
    __cpuid(r, 1);
    int fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1, movbe = (r[2] >> 22) & 1;
    __cpuidex(r, 7, 0);
    int avx2 = (r[1] >> 5) & 1, bmi1 = (r[1] >> 3) & 1, bmi2 = (r[1] >> 8) & 1;
    int ymm = osxsave && (_xgetbv(0) & 6) == 6;   /* the OS saves YMM state */
    return fma && avx && avx2 && bmi1 && bmi2 && movbe && ymm;
}
unsigned plat_cpu_count(void) { return (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }
void plat_os_version(char *out, size_t n) {
    typedef LONG (WINAPI *rgv_t)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v = {sizeof v};
    rgv_t rgv = (rgv_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&v);
    DWORD ubr = 0, sz = sizeof ubr; char disp[64] = "";
    DWORD dsz = sizeof disp;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD, NULL, &ubr, &sz);
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", RRF_RT_REG_SZ, NULL, disp, &dsz);
    snprintf(out, n, "Windows %lu.%lu build %lu.%lu%s%s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr, *disp ? " " : "", disp);
}
uint64_t plat_ram_bytes(void) { MEMORYSTATUSEX m = {sizeof m}; return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0; }

void plat_random(void *out, size_t n) {
    if (BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        uint8_t *p = out; uint64_t x = plat_qpc() ^ 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = (uint8_t)x; }
    }
}

/* ---- The parts of plat.h shared with the Linux port ---- */
#include <shlobj.h>
int plat_thr_start(plat_thr_t *t, plat_thread_fn fn, void *arg) { start_t *s = HeapAlloc(GetProcessHeap(), 0, sizeof *s); if (!s) return -1; s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_tramp, s, 0, NULL); if (!h) { HeapFree(GetProcessHeap(), 0, s); return -1; } *t = (plat_thr_t)h; return 0; }
void plat_thr_join(plat_thr_t t, unsigned ms) { WaitForSingleObject((HANDLE)t, ms); CloseHandle((HANDLE)t); }
void plat_thread_nice(int l) { SetThreadPriority(GetCurrentThread(), l == PLAT_PRIO_LOWEST ? THREAD_PRIORITY_LOWEST : l == PLAT_PRIO_BELOW_NORMAL ? THREAD_PRIORITY_BELOW_NORMAL : l == PLAT_PRIO_ABOVE_NORMAL ? THREAD_PRIORITY_ABOVE_NORMAL : l == PLAT_PRIO_HIGHEST ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL); }
plat_event *plat_event_new(int manual) { return (plat_event *)CreateEventW(NULL, manual, FALSE, NULL); }
void plat_event_set(plat_event *e) { SetEvent((HANDLE)e); }
void plat_event_reset(plat_event *e) { ResetEvent((HANDLE)e); }
int plat_event_wait(plat_event *e, unsigned ms) { return WaitForSingleObject((HANDLE)e, ms) == WAIT_OBJECT_0 ? 0 : 1; }
void plat_event_free(plat_event *e) { if (e) CloseHandle((HANDLE)e); }
plat_sem *plat_sem_new(void) { return (plat_sem *)CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL); }
void plat_sem_post(plat_sem *s) { ReleaseSemaphore((HANDLE)s, 1, NULL); }
int plat_sem_wait(plat_sem *s, unsigned ms) { return WaitForSingleObject((HANDLE)s, ms) == WAIT_OBJECT_0 ? 0 : 1; }
void plat_sem_free(plat_sem *s) { if (s) CloseHandle((HANDLE)s); }
_Noreturn void plat_exit(int code) { ExitProcess((UINT)code); }
void *plat_aligned_alloc(size_t size, size_t align) { return _aligned_malloc(size, align); }
void plat_aligned_free(void *p) { _aligned_free(p); }
static DWORD prot_of(int p) { return p == PLAT_MEM_RW ? PAGE_READWRITE : p == PLAT_MEM_RO ? PAGE_READONLY : p == PLAT_MEM_RX ? PAGE_EXECUTE_READ : PAGE_NOACCESS; }
void *plat_mem_reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }
void *plat_mem_zeroed(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void *plat_mem_near(uint64_t lo, uint64_t hi, size_t n) {
    for (uint64_t a = (lo + 0xffff) & ~0xffffull; a + n <= hi;) {
        void *p = VirtualAlloc((void *)(uintptr_t)a, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) break;
        a = ((uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize + 0xffff) & ~0xffffull;
    }
    return NULL;
}
int plat_mem_protect(void *a, size_t n, int prot) { DWORD old; return VirtualProtect(a, n, prot_of(prot), &old) ? 0 : -1; }
void plat_mem_free(void *a, size_t n) { (void)n; VirtualFree(a, 0, MEM_RELEASE); }
int plat_mem_query(const void *addr, uint64_t *end, size_t look) {
    (void)look; MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(addr, &m, sizeof m)) { if (end) *end = ((uint64_t)(uintptr_t)addr & ~0xfffull) + 0x1000; return 0; }
    if (end) *end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
    return m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
int plat_guarded(void (*fn)(void *), void *ctx) {
    __try { fn(ctx); return 0; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return -1; }
}
static int wide(const char *u, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n) > 0; }
void *plat_fopen(const char *path, const char *mode) { wchar_t p[1100], m[16]; if (!wide(path, p, 1100) || !wide(mode, m, 16)) return NULL; return _wfopen(p, m); }
int plat_mkdir(const char *path) { wchar_t w[1100]; if (!wide(path, w, 1100)) return -1; return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1; }
int plat_mkdirs(const char *path) { wchar_t w[1100]; if (!wide(path, w, 1100)) return -1; for (wchar_t *p = w + 3; *p; p++) if (*p == L'\\' || *p == L'/') { wchar_t c = *p; *p = 0; CreateDirectoryW(w, NULL); *p = c; } return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1; }
int plat_is_dir(const char *path) { wchar_t w[1100]; if (!wide(path, w, 1100)) return 0; DWORD a = GetFileAttributesW(w); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
int plat_is_file(const char *path) { wchar_t w[1100]; if (!wide(path, w, 1100)) return 0; DWORD a = GetFileAttributesW(w); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
int plat_replace_file(const char *from, const char *to) { wchar_t a[1100], b[1100]; if (!wide(from, a, 1100) || !wide(to, b, 1100)) return -1; return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1; }
int plat_unlink(const char *path) { wchar_t w[1100]; if (!wide(path, w, 1100)) return -1; return DeleteFileW(w) ? 0 : -1; }
int plat_write_file_durable(const char *path, const void *d, size_t n) {
    wchar_t w[1100]; if (!wide(path, w, 1100)) return -1;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0; int ok = WriteFile(h, d, (DWORD)n, &got, NULL) && got == n && FlushFileBuffers(h); CloseHandle(h); return ok ? 0 : -1;
}
int plat_cache_root(char *out, size_t n) {
    wchar_t *la = NULL; if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la) != S_OK) return -1;
    char u[900]; int ok = WideCharToMultiByte(CP_UTF8, 0, la, -1, u, sizeof u, NULL, NULL) > 0; CoTaskMemFree(la); if (!ok) return -1;
    snprintf(out, n, "%s\\pokopia-native", u); return plat_mkdirs(out);
}
int plat_exe_path(char *out, size_t n) { wchar_t w[1024]; DWORD k = GetModuleFileNameW(NULL, w, 1024); return k && k < 1024 && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0 ? 0 : -1; }
int plat_run_logged(const char *cmd, const char *log_path) {
    wchar_t wcmd[4096], wlog[1200]; if (!wide(cmd, wcmd, 4096) || !wide(log_path, wlog, 1200)) return -1;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE lh = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = si.hStdError = lh;
    DWORD code = 999;
    if (CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (lh != INVALID_HANDLE_VALUE) CloseHandle(lh);
    return (int)code;
}
void plat_load_vulkan_name(const char **name) { *name = "vulkan-1.dll"; }
void *plat_lib_open(const char *name) { return LoadLibraryA(name); }
void *plat_lib_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
const char *plat_last_error(void) { static _Thread_local char b[32]; snprintf(b, sizeof b, "error %lu", GetLastError()); return b; }
