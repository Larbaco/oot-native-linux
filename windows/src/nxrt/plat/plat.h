/* The platform layer of nxrt: everything that talks to the operating system apart from the address space (host/addrspace.c on Windows, host/addrspace_linux.c on
   Linux) and the POSIX subset the ported nxnative files use (posix_host.h: real POSIX on Linux, posix_win.h over Win32). Each system has its own implementation of
   this header (plat_win32.c, plat_linux.c), written against that system's own services. */
#ifndef NXRT_PLAT_H
#define NXRT_PLAT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clock: QueryPerformanceCounter in nanoseconds (monotonic, invariant and coherent across cores on Windows 10+) */
uint64_t plat_now_ns(void);
uint64_t plat_qpc(void);
uint64_t plat_qpc_freq(void);
uint64_t plat_realtime_ns(void);   /* since 1970, GetSystemTimePreciseAsFileTime */

/* Sleep with sub-millisecond resolution (a high-resolution waitable timer per thread) and yield */
void plat_sleep_ns(uint64_t ns);
void plat_yield(void);
static inline void plat_sleep_ms(unsigned ms) { plat_sleep_ns((uint64_t)ms * 1000000u); }
/* timeBeginPeriod(1) for the process, so condition-variable timeouts are millisecond-accurate */
void plat_timer_resolution(void);

/* Reads memory that may not be mapped: 0 when all n bytes were read, -1 otherwise (never faults) */
int plat_read(uint64_t addr, void *out, size_t n);

/* Threads */
typedef unsigned (*plat_thread_fn)(void *arg);
/* A detached thread with a stack reservation of stack_reserve bytes; returns the thread id, 0 on failure */
uint32_t plat_thread_start(plat_thread_fn fn, void *arg, size_t stack_reserve);
uint32_t plat_thread_id(void);
void plat_thread_name(const char *name);
/* Horizon priority (0 highest .. 63 lowest) to a Windows priority, gently (DESIGN D9: never above ABOVE_NORMAL) */
int plat_priority_for(int horizon_prio);
void plat_thread_set_priority(uint32_t tid, int horizon_prio);
double plat_thread_cpu_seconds(uint32_t tid);
_Noreturn void plat_thread_exit(void);

/* System information for the log banner */
void plat_cpu_brand(char *out, size_t n);
int plat_cpu_x86_64_v3(void);      /* AVX2 + FMA3 + BMI1/2 (DESIGN §9.1) */
unsigned plat_cpu_count(void);
void plat_os_version(char *out, size_t n);
uint64_t plat_ram_bytes(void);

/* Entropy (BCryptGenRandom) */
void plat_random(void *out, size_t n);

/* Threads that are joined (plat_thread_start's are detached). The function's result is ignored */
#ifdef _WIN32
typedef void *plat_thr_t;
#else
typedef unsigned long plat_thr_t;
#endif
int plat_thr_start(plat_thr_t *t, plat_thread_fn fn, void *arg);   /* 0 = started */
void plat_thr_join(plat_thr_t t, unsigned timeout_ms);             /* waits up to timeout_ms for the end, then lets go of the thread either way */

/* The calling thread's scheduling hint: a nudge, as far as the system lets an ordinary process go */
enum { PLAT_PRIO_LOWEST, PLAT_PRIO_BELOW_NORMAL, PLAT_PRIO_NORMAL, PLAT_PRIO_ABOVE_NORMAL, PLAT_PRIO_HIGHEST };
void plat_thread_nice(int level);

/* Events and counting semaphores */
typedef struct plat_event plat_event;
plat_event *plat_event_new(int manual_reset);
void plat_event_set(plat_event *e);
void plat_event_reset(plat_event *e);
int plat_event_wait(plat_event *e, unsigned timeout_ms);           /* 0 = signalled, 1 = timed out; PLAT_FOREVER waits for good */
void plat_event_free(plat_event *e);
typedef struct plat_sem plat_sem;
plat_sem *plat_sem_new(void);
void plat_sem_post(plat_sem *s);
int plat_sem_wait(plat_sem *s, unsigned timeout_ms);               /* 0 = taken, 1 = timed out */
void plat_sem_free(plat_sem *s);
#define PLAT_FOREVER 0xFFFFFFFFu

_Noreturn void plat_exit(int code);                                /* ends the process now, without atexit handlers */
void *plat_aligned_alloc(size_t size, size_t align);
void plat_aligned_free(void *p);

/* Memory outside the guest ranges */
enum { PLAT_MEM_NONE, PLAT_MEM_RO, PLAT_MEM_RW, PLAT_MEM_RX };
void *plat_mem_reserve(size_t n);                                  /* inaccessible address space */
void *plat_mem_zeroed(size_t n);                                   /* readable and writable, zero, backed by memory only where touched */
void *plat_mem_near(uint64_t lo, uint64_t hi, size_t n);           /* read-write, somewhere in [lo, hi) */
int plat_mem_protect(void *a, size_t n, int prot);                 /* 0 = done */
void plat_mem_free(void *a, size_t n);
/* Whether addr can be read, and up to where (exclusive) that answer holds as far as was looked: never below addr + 1 page */
int plat_mem_query(const void *addr, uint64_t *end, size_t look);
/* Runs fn(ctx) with access faults on unmapped or protected memory caught, after the fault hooks: 0 = it ran to its end, -1 = it faulted and was cut off.
   fn must hold no lock and own nothing it would have to release: nothing after the fault runs */
int plat_guarded(void (*fn)(void *), void *ctx);

/* Files, by UTF-8 path */
#ifdef _WIN32
#define PLAT_SEP "\\"
#else
#define PLAT_SEP "/"
#endif
void *plat_fopen(const char *path, const char *mode);              /* FILE *, binary as given by mode */
int plat_mkdir(const char *path);                                  /* 0 = made or there already */
int plat_mkdirs(const char *path);
int plat_is_dir(const char *path);
int plat_is_file(const char *path);
int plat_replace_file(const char *from, const char *to);           /* atomic, durable where the system offers it; 0 = done */
int plat_unlink(const char *path);
int plat_write_file_durable(const char *path, const void *d, size_t n);   /* written and flushed to the disk; 0 = done */
int plat_cache_root(char *out, size_t n);                          /* the per-user folder for pokopia-native's caches, made if needed; 0 = done */
int plat_exe_path(char *out, size_t n);                            /* this executable; 0 = done */
/* Runs a command line (a program and its quoted arguments) at below-normal priority with stdout and stderr in a log file; the exit code, or -1 */
int plat_run_logged(const char *cmd, const char *log_path);
void plat_load_vulkan_name(const char **name);                     /* the Vulkan loader's library name */
void *plat_lib_open(const char *name);
void *plat_lib_sym(void *lib, const char *name);
const char *plat_last_error(void);                                 /* for a log line, after a failed call here or in the address space */

#ifdef __cplusplus
}
#endif
#endif
