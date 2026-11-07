/* Test "guests": host functions registered in the code table at guest addresses, running on guest threads with their own NxwGuestCpu, using the Horizon
   kernel only through rt_svc, the way the SDK's nn::os does (lock words tagged with thread handles, condition keys, address arbitration, events). */
#pragma once
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"

extern int gt_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); gt_fail = 1; } } while (0)

#define R_TIMEOUT 0xEA01u
#define R_CANCELLED 0xEC01u
#define R_BAD_HANDLE 0xE401u
#define R_BAD_STATE 0xFA01u

#include "plat.h"
static inline uint64_t now_ns_test(void) { return plat_now_ns(); }
void gt_init(void);                          /* nxrt_init, a code table over fake guest code, the calling thread as the guest main thread, a 64 MB heap */
uint64_t gt_fn(NxwGuestFn fn);               /* fn at the next fake guest code address */
uint64_t gt_svc(NxwGuestCpu *c, uint32_t n); /* rt_svc, returns x0 */
void *gt_alloc(size_t n);                    /* from the guest heap, 64-byte aligned, zeroed */
extern uint64_t gt_heap, gt_heap_size;
uint32_t gt_self(void);                      /* the calling guest thread's handle (its lock tag) */
/* nn::os-style primitives */
void gt_lock(NxwGuestCpu *c, _Atomic uint32_t *w);
void gt_unlock(NxwGuestCpu *c, _Atomic uint32_t *w);
uint32_t gt_cond_wait(NxwGuestCpu *c, _Atomic uint32_t *lock, _Atomic uint32_t *key, int64_t timeout_ns);   /* returns with the lock held */
void gt_cond_signal(NxwGuestCpu *c, _Atomic uint32_t *key, int count);
/* A guest thread: its stack from the heap, moved into the stack region with MapMemory as the SDK does; started at once */
typedef struct { uint32_t handle; uint64_t stack_src, stack_dst, stack_size; } gt_thread_t;
gt_thread_t gt_thread(NxwGuestCpu *c, NxwGuestFn entry, uint64_t arg, int prio, int core);
void gt_join(NxwGuestCpu *c, gt_thread_t *t);   /* WaitSynchronization on the handle, CloseHandle, UnmapMemory */
