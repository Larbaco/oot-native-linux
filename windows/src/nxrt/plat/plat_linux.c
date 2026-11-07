/* Linux implementation of plat.h: the kernel's and glibc's own services (clock_gettime, clock_nanosleep, process_vm_readv, pthreads, mmap, sigsetjmp). */
#define _GNU_SOURCE
#include "plat.h"
#include "game_profile.h"

#include <cpuid.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
uint64_t plat_qpc(void) { return plat_now_ns(); }
uint64_t plat_qpc_freq(void) { return 1000000000u; }
uint64_t plat_realtime_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }

void plat_sleep_ns(uint64_t ns) {
    if (!ns) { sched_yield(); return; }
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR) {}
}
void plat_yield(void) { sched_yield(); }
void plat_timer_resolution(void) {}   /* clock_nanosleep is high-resolution already */

int plat_read(uint64_t addr, void *out, size_t n) {
    if (!addr) return -1;
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

typedef struct { plat_thread_fn fn; void *arg; } start_t;
static void *thread_tramp(void *p) {
    start_t s = *(start_t *)p; free(p);
    s.fn(s.arg);
    return NULL;
}
static int start_pthread(pthread_t *t, plat_thread_fn fn, void *arg, size_t stack, int detached) {
    start_t *s = malloc(sizeof *s);
    if (!s) return EAGAIN;
    s->fn = fn; s->arg = arg;
    pthread_attr_t a; pthread_attr_init(&a);
    if (stack) pthread_attr_setstacksize(&a, stack);
    if (detached) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(t, &a, thread_tramp, s);
    pthread_attr_destroy(&a);
    if (rc) free(s);
    return rc;
}
/* The id comes from the thread itself, so the start waits for it */
typedef struct { plat_thread_fn fn; void *arg; sem_t up; _Atomic uint32_t tid; } idstart_t;
static unsigned idstart_tramp(void *p) {
    idstart_t *s = p; plat_thread_fn fn = s->fn; void *arg = s->arg;
    s->tid = plat_thread_id(); sem_post(&s->up);
    return fn(arg);
}
uint32_t plat_thread_start(plat_thread_fn fn, void *arg, size_t stack_reserve) {
    idstart_t s; s.fn = fn; s.arg = arg; s.tid = 0;
    sem_init(&s.up, 0, 0);
    pthread_t t;
    if (start_pthread(&t, idstart_tramp, &s, stack_reserve, 1)) { sem_destroy(&s.up); return 0; }
    while (sem_wait(&s.up) && errno == EINTR) {}
    sem_destroy(&s.up);
    return s.tid;
}
uint32_t plat_thread_id(void) { static _Thread_local uint32_t id; if (!id) id = (uint32_t)syscall(SYS_gettid); return id; }
void plat_thread_name(const char *name) { char n[16]; snprintf(n, sizeof n, "%s", name); pthread_setname_np(pthread_self(), n); }
/* Horizon priorities run 0 (highest) to 63; this game uses 28..59. Linux: nice values for this thread, only ever a nudge: a process may lower itself freely and may
   raise itself only as far as its RLIMIT_NICE allows, and a refused raise leaves the thread at normal */
static int nice_of_level(int l) { return l == PLAT_PRIO_LOWEST ? 10 : l == PLAT_PRIO_BELOW_NORMAL ? 3 : l == PLAT_PRIO_ABOVE_NORMAL ? -3 : l == PLAT_PRIO_HIGHEST ? -6 : 0; }
int plat_priority_for(int p) { return p <= 27 ? PLAT_PRIO_ABOVE_NORMAL : p <= 44 ? PLAT_PRIO_NORMAL : p <= 59 ? PLAT_PRIO_BELOW_NORMAL : PLAT_PRIO_LOWEST; }
void plat_thread_set_priority(uint32_t tid, int horizon_prio) { setpriority(PRIO_PROCESS, tid, nice_of_level(plat_priority_for(horizon_prio))); }
void plat_thread_nice(int level) { setpriority(PRIO_PROCESS, plat_thread_id(), nice_of_level(level)); }
double plat_thread_cpu_seconds(uint32_t tid) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%u/schedstat", tid);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    unsigned long long ns = 0; int ok = fscanf(f, "%llu", &ns) == 1; fclose(f);
    return ok ? ns / 1e9 : -1;
}
_Noreturn void plat_thread_exit(void) { pthread_exit(NULL); }

int plat_thr_start(plat_thr_t *t, plat_thread_fn fn, void *arg) { pthread_t th; int rc = start_pthread(&th, fn, arg, 0, 0); if (!rc) *t = (plat_thr_t)th; return rc; }
void plat_thr_join(plat_thr_t t, unsigned timeout_ms) {
    struct timespec d; clock_gettime(CLOCK_REALTIME, &d);
    d.tv_sec += timeout_ms / 1000; d.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
    if (pthread_timedjoin_np((pthread_t)t, NULL, &d)) pthread_detach((pthread_t)t);
}

struct plat_event { pthread_mutex_t m; pthread_cond_t c; int on, manual; };
plat_event *plat_event_new(int manual_reset) {
    plat_event *e = calloc(1, sizeof *e); if (!e) return NULL;
    pthread_mutex_init(&e->m, NULL);
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&e->c, &a); pthread_condattr_destroy(&a);
    e->manual = manual_reset; return e;
}
void plat_event_set(plat_event *e) { pthread_mutex_lock(&e->m); e->on = 1; if (e->manual) pthread_cond_broadcast(&e->c); else pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m); }
void plat_event_reset(plat_event *e) { pthread_mutex_lock(&e->m); e->on = 0; pthread_mutex_unlock(&e->m); }
int plat_event_wait(plat_event *e, unsigned ms) {
    struct timespec d; clock_gettime(CLOCK_MONOTONIC, &d);
    d.tv_sec += ms / 1000; d.tv_nsec += (long)(ms % 1000) * 1000000;
    if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
    int rc = 0;
    pthread_mutex_lock(&e->m);
    while (!e->on && !rc) rc = ms == PLAT_FOREVER ? pthread_cond_wait(&e->c, &e->m) : pthread_cond_timedwait(&e->c, &e->m, &d);
    int got = e->on; if (got && !e->manual) e->on = 0;
    pthread_mutex_unlock(&e->m);
    return got ? 0 : 1;
}
void plat_event_free(plat_event *e) { if (e) { pthread_cond_destroy(&e->c); pthread_mutex_destroy(&e->m); free(e); } }

struct plat_sem { sem_t s; };
plat_sem *plat_sem_new(void) { plat_sem *s = malloc(sizeof *s); if (s && sem_init(&s->s, 0, 0)) { free(s); s = NULL; } return s; }
void plat_sem_post(plat_sem *s) { sem_post(&s->s); }
int plat_sem_wait(plat_sem *s, unsigned ms) {
    if (ms == PLAT_FOREVER) { while (sem_wait(&s->s)) if (errno != EINTR) return 1; return 0; }
    struct timespec d; clock_gettime(CLOCK_MONOTONIC, &d);
    d.tv_sec += ms / 1000; d.tv_nsec += (long)(ms % 1000) * 1000000;
    if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
    while (sem_clockwait(&s->s, CLOCK_MONOTONIC, &d)) if (errno != EINTR) return 1;
    return 0;
}
void plat_sem_free(plat_sem *s) { if (s) { sem_destroy(&s->s); free(s); } }

_Noreturn void plat_exit(int code) { fflush(NULL); _exit(code); }
void *plat_aligned_alloc(size_t size, size_t align) { void *p = NULL; return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p; }
void plat_aligned_free(void *p) { free(p); }

static int prot_of(int p) { return p == PLAT_MEM_RW ? PROT_READ | PROT_WRITE : p == PLAT_MEM_RO ? PROT_READ : p == PLAT_MEM_RX ? PROT_READ | PROT_EXEC : PROT_NONE; }
void *plat_mem_reserve(size_t n) { void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
void *plat_mem_zeroed(size_t n) { void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return p == MAP_FAILED ? NULL : p; }
void *plat_mem_near(uint64_t lo, uint64_t hi, size_t n) {
    for (uint64_t a = (lo + 0xffff) & ~0xffffull; a + n <= hi; a += 0x1000000) {
        void *p = mmap((void *)(uintptr_t)a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)(uintptr_t)a) return p;
        if (p != MAP_FAILED) munmap(p, n);
    }
    return NULL;
}
int plat_mem_protect(void *a, size_t n, int prot) { return mprotect(a, n, prot_of(prot)) ? -1 : 0; }
void plat_mem_free(void *a, size_t n) { munmap(a, n); }
int plat_mem_query(const void *addr, uint64_t *end, size_t look) {
    enum { P = 4096, BATCH = 256 };
    uint64_t a = (uint64_t)(uintptr_t)addr & ~(uint64_t)(P - 1), stop = a + (look ? look : P);
    char first, sink[BATCH]; struct iovec l = {&first, 1}, r = {(void *)(uintptr_t)a, 1};
    int readable = process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 1;
    uint64_t q = a + P;
    while (q < stop) {   /* one byte per page: the read stops at the first page that cannot be read */
        struct iovec li[BATCH], ri[BATCH]; int k = 0;
        for (; k < BATCH && q + (uint64_t)k * P < stop; k++) { li[k] = (struct iovec){sink + k, 1}; ri[k] = (struct iovec){(void *)(uintptr_t)(q + (uint64_t)k * P), 1}; }
        if (readable) {
            ssize_t got = process_vm_readv(getpid(), li, (unsigned long)k, ri, (unsigned long)k, 0);
            if (got < k) { q += (uint64_t)(got < 0 ? 0 : got) * P; break; }
            q += (uint64_t)k * P;
        } else {
            int i = 0;
            for (; i < k; i++) if (process_vm_readv(getpid(), li + i, 1, ri + i, 1, 0) == 1) break;
            q += (uint64_t)i * P;
            if (i < k) break;
        }
    }
    if (end) *end = q < stop ? q : stop;
    return readable;
}

/* The fault handler (host/crash.c) jumps back here. The handler runs with SA_NODEFER, so no signal mask is left to restore: sigsetjmp saves none, which keeps a guarded
   copy to a few instructions more than the plain one */
_Thread_local void *plat_guard_jmp;
int plat_guarded(void (*fn)(void *), void *ctx) {
    sigjmp_buf jb; void *prev = plat_guard_jmp; int rc = 0;
    if (!sigsetjmp(jb, 0)) { plat_guard_jmp = &jb; fn(ctx); }
    else rc = -1;
    plat_guard_jmp = prev;
    return rc;
}

void *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int plat_mkdir(const char *path) { return mkdir(path, 0777) && errno != EEXIST ? -1 : 0; }
int plat_mkdirs(const char *path) {
    char b[PATH_MAX]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(b, 0777); *p = '/'; }
    return plat_mkdir(b);
}
int plat_is_dir(const char *path) { struct stat s; return !stat(path, &s) && S_ISDIR(s.st_mode); }
int plat_is_file(const char *path) { struct stat s; return !stat(path, &s) && !S_ISDIR(s.st_mode); }
int plat_replace_file(const char *from, const char *to) { return rename(from, to) ? -1 : 0; }
int plat_unlink(const char *path) { return unlink(path); }
int plat_write_file_durable(const char *path, const void *d, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); if (fd < 0) return -1;
    const char *p = d; size_t left = n;
    while (left) { ssize_t w = write(fd, p, left); if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; } p += w; left -= (size_t)w; }
    int rc = fsync(fd); close(fd); return rc ? -1 : 0;
}
int plat_cache_root(char *out, size_t n) {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/" DM_GAME_SLUG "-native", x);
    else if (h && *h) snprintf(out, n, "%s/.cache/" DM_GAME_SLUG "-native", h);
    else return -1;
    return plat_mkdirs(out);
}
int plat_exe_path(char *out, size_t n) { ssize_t k = readlink("/proc/self/exe", out, n - 1); if (k <= 0) return -1; out[k] = 0; return 0; }
int plat_run_logged(const char *cmd, const char *log_path) {
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    char *argv[] = {"sh", "-c", (char *)cmd, NULL}; pid_t pid = 0;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) return -1;
    setpriority(PRIO_PROCESS, (id_t)pid, 5);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
void plat_load_vulkan_name(const char **name) { *name = "libvulkan.so.1"; }
void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return dlsym(lib, name); }
const char *plat_last_error(void) { return strerror(errno); }

void plat_cpu_brand(char *out, size_t n) {
    unsigned r[4]; char b[49] = {0};
    if (__get_cpuid_max(0x80000000u, NULL) >= 0x80000004u)
        for (unsigned i = 0; i < 3; i++) { __cpuid(0x80000002u + i, r[0], r[1], r[2], r[3]); memcpy(b + 16 * i, r, 16); }
    const char *s = b; while (*s == ' ') s++;
    snprintf(out, n, "%s", *s ? s : "unknown x86-64 CPU");
}
int plat_cpu_x86_64_v3(void) {
    unsigned a, b, c, d;
    __cpuid(1, a, b, c, d);
    int fma = (c >> 12) & 1, osxsave = (c >> 27) & 1, avx = (c >> 28) & 1, movbe = (c >> 22) & 1;
    __cpuid_count(7, 0, a, b, c, d);
    int avx2 = (b >> 5) & 1, bmi1 = (b >> 3) & 1, bmi2 = (b >> 8) & 1, ymm = 0;
    if (osxsave) { unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); ymm = (lo & 6) == 6; }
    return fma && avx && avx2 && bmi1 && bmi2 && movbe && ymm;
}
unsigned plat_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)n : 1; }
void plat_os_version(char *out, size_t n) { struct utsname u; if (uname(&u)) snprintf(out, n, "Linux"); else snprintf(out, n, "%s %s %s", u.sysname, u.release, u.machine); }
uint64_t plat_ram_bytes(void) { struct sysinfo s; return sysinfo(&s) ? 0 : (uint64_t)s.totalram * s.mem_unit; }
void plat_random(void *out, size_t n) {
    uint8_t *p = out;
    while (n) { ssize_t k = getrandom(p, n, 0); if (k < 0) { if (errno == EINTR) continue; break; } p += k; n -= (size_t)k; }
    if (n) { uint64_t x = plat_now_ns() ^ 0x9E3779B97F4A7C15ull; for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = (uint8_t)x; } }
}
