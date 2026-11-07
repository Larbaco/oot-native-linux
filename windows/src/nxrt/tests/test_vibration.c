/* The offline vibration graph still has guest C++ object lifetime semantics.
   Exercise the real HLE adapter with a synthetic SDK vtable, no game/device. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "hle_adapters.h"

static unsigned failures, destroyed, lookups;
#define T(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
typedef struct connection {
    uintptr_t vptr, prev, next, source, destination;
    float modulation[4];
} connection;
_Static_assert(sizeof(connection) == 0x38, "SDK vibration connection ABI");
static void destroy(connection *c) {
    T(c->prev == (uintptr_t)&c->prev && c->next == c->prev);
    T(c->source == 0 && c->destination == 0);
    destroyed++;
}
static uintptr_t sdk_vtable[] = {0, 0, (uintptr_t)destroy, 0};
/* Only symbol resolution is replaced; the constructor adapter is production code. */
void *__wrap_guest_sym(const char *name) {
    T(!strcmp(name, "_ZTVN2nn3hid23VibrationNodeConnectionE"));
    lookups++;
    return sdk_vtable;
}
int main(void) {
    struct { uint64_t before; connection slots[2]; uint64_t after; } pool;
    memset(&pool, 0xa5, sizeof pool);
    pool.before = UINT64_C(0x123456789abcdef0);
    pool.after = UINT64_C(0x0fedcba987654321);
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        /* A real pool overlays its free-list link on the old vptr. Constructing
           it must replace that link before a later virtual destructor call. */
        pool.slots[0].vptr = (uintptr_t)&pool.slots[1];
        pool.slots[1].vptr = 0;
        for (unsigned i = 0; i < 2; i++) {
            NxwGuestCpu cpu = {0}; cpu.x[0] = (uintptr_t)&pool.slots[i];
            adapter__ZN2nn3hid23VibrationNodeConnectionC1Ev(&cpu);
            connection *c = &pool.slots[i];
            T(c->vptr == (uintptr_t)&sdk_vtable[2]);
            for (unsigned j = 0; j < 4; j++) T(c->modulation[j] == 1.0f);
            if (c->vptr == (uintptr_t)&sdk_vtable[2]) {
                ((void (*)(connection *))(*(uintptr_t *)c->vptr))(c);
            }
        }
    }
    T(destroyed == 6 && lookups > 0);
    T(pool.before == UINT64_C(0x123456789abcdef0));
    T(pool.after == UINT64_C(0x0fedcba987654321));
    printf("vibration pooled object lifetime: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
