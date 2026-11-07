// from nxnative dm/host/thread.c (MIT, © seedds): a guest thread's environment. tpidr_el0 = the ELF TLS block (8 KB); tpidrro_el0 = the
// Horizon ThreadLocalRegion (0x200) followed by the thread object (0x400). The kernel owns two fields of the ThreadLocalRegion that the SDK reads:
// the current thread object at TLR[0x1f8] and, since SDK 22, the thread's ID at TLR[0x110] (nn::os 22's internal critical section stores it as a
// mutex's owner and compares it in IsLockedByCurrentThread).
// Windows: the two registers are fields of the thread's NxwGuestCpu (research/03 §3.7), not TSD slots behind a key page; the gate stacks vanish
// (host code runs on the host stack). The blocks are freed when the thread exits (guest_thread_free).
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400

typedef struct { void *tls, *tlr; } guest_tls_t;

// Fills cpu->tpidr_el0 / tpidrro_el0 with fresh blocks; returns the allocation to free at exit
void *guest_thread_init(NxwGuestCpu *cpu) {
    guest_tls_t *own = calloc(1, sizeof *own);
    uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    if (!own || !tlr) die("guest thread-local block allocation failed");
    own->tls = calloc(1, TLS_SIZE); own->tlr = tlr;
    if (!own->tls) die("guest thread TLS block allocation failed");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // current thread object
    cpu->tpidr_el0 = (uint64_t)(uintptr_t)own->tls;
    cpu->tpidrro_el0 = (uint64_t)(uintptr_t)tlr;
    return own;
}
void guest_thread_free(void *p) { guest_tls_t *own = p; if (!own) return; free(own->tls); free(own->tlr); free(own); }
// The thread's kernel ID at TLR[0x110], the one svcGetThreadId answers (hle_thread.c's thr_t id, never 0: 0 is the owner word of an unlocked mutex). Without it every
// unlocked SDK 22 mutex reads as the caller's and the first heap lock aborts
void guest_thread_set_id(NxwGuestCpu *cpu, uint64_t id) { *(uint32_t *)((uint8_t *)(uintptr_t)cpu->tpidrro_el0 + 0x110) = (uint32_t)id; }
