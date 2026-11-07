/* Mutexes, condition variables, keys, threads and clocks under their POSIX names on every system. */
#pragma once
#ifdef _WIN32
#include "posix_win.h"
/* A timed wait on a mutex and a condition variable, in milliseconds */
static inline void plat_cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, unsigned ms) { SleepConditionVariableSRW(c, m, ms, 0); }
/* The C library's time-zone name, for the two places that read it */
#define plat_tzname(i) (_tzname[i])
#else
#include "posix_linux.h"
#define plat_tzname(i) (tzname[i])
static inline void plat_cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, unsigned ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(c, m, &t);
}
#endif

/* Reader-writer locks. Writers do not starve behind a stream of readers on either system */
#ifdef _WIN32
typedef SRWLOCK plat_rwlock_t;
#define PLAT_RWLOCK_INIT SRWLOCK_INIT
#define PLAT_RWLOCK_RP_INIT SRWLOCK_INIT   // see the POSIX branch
#define plat_rw_init InitializeSRWLock
#define plat_rw_wrlock AcquireSRWLockExclusive
#define plat_rw_rdlock AcquireSRWLockShared
#define plat_rw_wrunlock ReleaseSRWLockExclusive
#define plat_rw_rdunlock ReleaseSRWLockShared
#else
typedef pthread_rwlock_t plat_rwlock_t;
#define PLAT_RWLOCK_INIT PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
// Readers first: a reader gets in while a writer waits (glibc's default kind). For a lock whose readers can wait for the GPU while holding it and whose completion-thread effects need
// it for reading too: with writers first, one queued writer blocks that effect, the GPU's timeline stops advancing on our side and the reader waits for it for ever (2026-11-05)
#define PLAT_RWLOCK_RP_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void plat_rw_init(plat_rwlock_t *l) {
    pthread_rwlockattr_t a; pthread_rwlockattr_init(&a); pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(l, &a); pthread_rwlockattr_destroy(&a);
}
#define plat_rw_wrlock pthread_rwlock_wrlock
#define plat_rw_rdlock pthread_rwlock_rdlock
#define plat_rw_wrunlock pthread_rwlock_unlock
#define plat_rw_rdunlock pthread_rwlock_unlock
#endif
