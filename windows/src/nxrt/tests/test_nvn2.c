/* NVN2 adapter ABI checks without game files, a GPU, or a display. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "hle_adapters.h"
#include "hle_registry.h"
#include "nvn_obj.h"

static int failures;
#define T(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define P(p) ((uint64_t)(uintptr_t)(p))
static uint64_t call(void (*fn)(NxwGuestCpu *), uint64_t a, uint64_t b) {
    NxwGuestCpu cpu = {0}; cpu.x[0] = a; cpu.x[1] = b;
    fn(&cpu); return cpu.x[0];
}

int main(void) {
    if (nxrt_init(&(nxrt_options){.quiet = 1})) return 2;
    NVNwindowBuilder builder = {0}; NVNwindow window = {0};
    call(adapter_nvnWindowBuilderSetDefaults, P(&builder), 0);
    T(call(adapter_nvnWindowBuilderGetAcquireTimeout, P(&builder), 0) == UINT64_MAX);
    const uint64_t timeout = UINT64_C(0x123456789abcdef0);
    call(adapter_nvnWindowBuilderSetAcquireTimeout, P(&builder), timeout);
    T(call(adapter_nvnWindowBuilderGetAcquireTimeout, P(&builder), 0) == timeout);
    T(call(adapter_nvnWindowInitialize, P(&window), P(&builder)) == 1);
    T(call(adapter_nvnWindowGetAcquireTimeout, P(&window), 0) == timeout);
    call(adapter_nvnWindowSetAcquireTimeout, P(&window), 0);
    T(call(adapter_nvnWindowGetAcquireTimeout, P(&window), 0) == 0);
    call(adapter_nvnWindowSetAcquireTimeout, P(&window), UINT64_MAX);
    T(call(adapter_nvnWindowGetAcquireTimeout, P(&window), 0) == UINT64_MAX);
    T(call(adapter_nvnWindowBuilderGetAcquireTimeout, P(&builder), 0) == timeout);
    call(adapter_nvnWindowBuilderSetDefaults, P(&builder), 0);
    T(call(adapter_nvnWindowBuilderGetAcquireTimeout, P(&builder), 0) == UINT64_MAX);

    const char *pending[] = {"nvnTexturePoolWriteDescriptorTableData", "nvnSamplerPoolWriteDescriptorTableData",
        "nvnCommandBufferConvertCooperativeVectorMatrix", "nvnDeviceGetCooperativeVectorMatrixSize", "nvnDeviceConvertCooperativeVectorMatrix"};
    for (unsigned i = 0; i < sizeof pending / sizeof *pending; i++) {
        const nxrt_hle_entry *e = nxrt_hle_find(pending[i], NXRT_HLE_NVN);
        T(e && !e->ported);
        T(call(adapter_nvnDeviceGetProcAddress, 0, P(pending[i])) != 0);
    }
    T(call(adapter_nvnDeviceGetProcAddress, 0, P("nvnDefinitelyNotAnAPI")) == 0);
    const char *hints[] = {"nvnQueueBuilderSetPrimaryQueue", "nvnCommandBufferSetRecordingMode",
        "nvnCommandBufferSetComputeOverlapMode", "nvnCommandBufferSetComputeWorkloadPriority"};
    for (unsigned i = 0; i < sizeof hints / sizeof *hints; i++) {
        const nxrt_hle_entry *e = nxrt_hle_find(hints[i], NXRT_HLE_NVN);
        T(e && e->ported);
    }
    // Neither optional scheduling hints nor unknown-ABI placeholders may touch
    // arguments: the serial executor has no queue topology to configure.
    call(adapter_nvnQueueBuilderSetPrimaryQueue, 1, UINT64_MAX);
    call(adapter_nvnCommandBufferSetRecordingMode, 1, 2);
    call(adapter_nvnCommandBufferSetComputeOverlapMode, 1, 0);
    call(adapter_nvnCommandBufferSetComputeWorkloadPriority, 1, 32);
    T(call(adapter_nvnTexturePoolWriteDescriptorTableData, 1, UINT64_MAX) == 0);
    T(call(adapter_nvnDeviceGetCooperativeVectorMatrixSize, 1, UINT64_MAX) == 0);
    printf("NVN2 adapters: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
