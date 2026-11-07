/* Address-space reservation at the real bases (DESIGN D3): the executable's TLS callback reserved the token, module and guest ranges as placeholders,
   /HIGHENTROPYVA:NO took effect, nothing the host allocates lands below 2^39, a placeholder can be split and replaced at 4 KB granularity, and the
   guest section aliases: a view mapped at a second address sees the same bytes (svcMapMemory's real alias). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "addrspace.h"
#include "plat.h"
#include "nxrt/runtime.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s (error %lu)\n", __FILE__, __LINE__, #c, GetLastError()); g_fail = 1; } } while (0)

static void query(const char *what, uint64_t a) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) { printf("  %s %#llx: VirtualQuery failed\n", what, (unsigned long long)a); return; }
    printf("  %s %#llx: region %p+%#zx alloc %p state %#lx type %#lx protect %#lx\n", what, (unsigned long long)a, m.BaseAddress, m.RegionSize, m.AllocationBase, m.State, m.Type, m.Protect);
}

/* The fallback's mechanism, and the detection: a parent creates this test suspended, reserves 64 KB inside the child's token range with
   VirtualAllocEx before any of the child's code (or its DLLs) runs, and resumes it. The child's TLS callback must then report that range as taken,
   by whom and where. A launcher does the same with the guest ranges themselves: reserve them in the suspended child, then let it run. */
static int child(void) {
    const nxrt_range *r = nxrt_as_range(NXRT_AS_TOKENS);
    printf("  child: tokens %s, conflict %#llx +%#llx state %#x type %#x\n", r->reserved ? "reserved" : "NOT reserved", (unsigned long long)r->conflict_base,
           (unsigned long long)r->conflict_size, r->conflict_state, r->conflict_type);
    return !r->reserved && r->conflict_base == NXRT_TOKEN_BASE + 0x10000 && r->conflict_state == MEM_RESERVE && nxrt_as_range(NXRT_AS_MODULES)->reserved ? 0 : 1;
}
static int spawn_child(void) {
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --child", exe);
    STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) return -1;
    void *p = VirtualAllocEx(pi.hProcess, (void *)(uintptr_t)(NXRT_TOKEN_BASE + 0x10000), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return p ? (int)code : -2;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--child")) return child();
    /* 1. the reservation happened before main, at the real bases */
    for (int i = 0; i < NXRT_AS_COUNT; i++) {
        const nxrt_range *r = nxrt_as_range(i);
        printf("%-34s %#llx +%#llx %s\n", r->name, (unsigned long long)r->base, (unsigned long long)r->size, r->reserved ? "reserved" : "NOT reserved");
        CHECK(r->reserved);
        query("start", r->base);
        CHECK(r->base + r->size <= (1ull << 39));   /* 39-bit guest pointers */
    }
    CHECK(nxrt_as_range(NXRT_AS_MODULES)->base == 0x7100000000ull);
    const nxrt_layout *l = nxrt_as_layout();
    CHECK(l->as_size == 0x2c0000000ull && l->alias == l->as_base + 0x10000000 && l->alias_size == 0x10000000);
    CHECK(l->stack == l->alias + l->alias_size && l->heap == l->stack + l->stack_size && l->heap + l->heap_size == l->as_base + l->as_size);

    /* 2. /HIGHENTROPYVA:NO took effect, and host allocations stay out of the guest range */
    uint8_t *img = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    CHECK(!(nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA));
    void *v = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_NOACCESS), *h = malloc(64 << 20);
    printf("host allocations: VirtualAlloc %p, malloc(64 MB) %p\n", v, h);
    CHECK((uint64_t)(uintptr_t)v < NXRT_TOKEN_BASE || (uint64_t)(uintptr_t)v >= (1ull << 39));
    CHECK((uint64_t)(uintptr_t)h < NXRT_TOKEN_BASE || (uint64_t)(uintptr_t)h >= (1ull << 39));
    free(h); VirtualFree(v, 0, MEM_RELEASE);

    /* 3. a 4 KB piece of the module placeholder becomes private memory, and goes back */
    uint64_t a = NXRT_MODULE_BASE + 0x123000;
    int rc = as_commit_private(a, 0x3000, PAGE_READWRITE);
    if (rc) { printf("  as_commit_private: step %d\n", rc); query("piece", a); }
    CHECK(rc == 0);
    if (!rc) {
        memset((void *)(uintptr_t)a, 0x5a, 0x3000);
        CHECK(*(volatile uint8_t *)(uintptr_t)(a + 0x2fff) == 0x5a);
        query("piece", a);
        CHECK(as_release_private(a, 0x3000) == 0);
    }

    /* 4. the guest section: alias and heap views; a heap range mapped a second time into the stack region at 4 KB granularity */
    CHECK(as_guest_views() == 0);
    uint64_t src = l->heap + 0x5000, dst = l->stack + 0x3000, off = 0;
    CHECK(as_commit(src, 0x2000) == 0);
    memcpy((void *)(uintptr_t)src, "guest heap bytes", 17);
    CHECK(as_section_offset(src, 0x2000, &off) == 0 && off == src - l->alias);
    rc = as_map_view(dst, 0x2000, off);
    if (rc) query("dst", dst);
    CHECK(rc == 0);
    if (!rc) {
        CHECK(as_commit(dst, 0x2000) == 0);
        CHECK(!memcmp((void *)(uintptr_t)dst, "guest heap bytes", 17));                 /* the same pages */
        ((char *)(uintptr_t)dst)[0] = 'G';
        CHECK(((char *)(uintptr_t)src)[0] == 'G');
        CHECK(as_unmap_view(dst) == 0);
        as_coalesce(l->stack, l->stack + l->stack_size);
        query("stack region after unmap", l->stack);
        CHECK(as_map_view(dst, 0x2000, off) == 0 && as_unmap_view(dst) == 0);   /* the gap can be split again */
    }
    /* 5. guest memory nothing mapped faults (the null page, the unused first 256 MB of the block) */
    CHECK(plat_read(l->as_base, &rc, 4) != 0);
    CHECK(plat_read(0, &rc, 4) != 0);
    /* 6. a range taken before the runtime runs is detected and described */
    int ch = spawn_child();
    printf("child with its token range taken first: %s\n", ch == 0 ? "detected" : "NOT detected");
    CHECK(ch == 0);
    printf(g_fail ? "nxrt.addrspace: FAILED\n" : "nxrt.addrspace: ok\n");
    return g_fail;
}
