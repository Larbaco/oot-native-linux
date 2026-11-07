/* vk/vk_thandle.h: the texture ID of a descriptor handle against the pool: in range, empty, a header byte offset (index * 32, Eglantine 049c9633) and out of range
   (an empty descriptor, Lichii b34b24cd). Needs no Vulkan device. */
#include <stdio.h>
#include "vk_thandle.h"

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void) {
    int k, id;
    id = vkt_handle_index(0x00300123, 12544, &k);   T(id == 0x123 && k == VKH_OK);            /* the sampler ID above bit 20 is ignored */
    id = vkt_handle_index(0, 12544, &k);            T(id == 0 && k == VKH_EMPTY);
    id = vkt_handle_index(12543, 12544, &k);        T(id == 12543 && k == VKH_OK);            /* the last entry */
    id = vkt_handle_index(12545, 12544, &k);        T(id == 0 && k == VKH_RANGE);             /* first ID past the pool that is not a multiple of 32 */
    id = vkt_handle_index(12544, 12544, &k);        T(id == 392 && k == VKH_OFFSET);          /* 12544 = 392 * 32: an offset into a pool of 12544 entries */
    id = vkt_handle_index(1000 * 32, 12544, &k);    T(id == 1000 && k == VKH_OFFSET);         /* index 1000 as a byte offset into the descriptor table */
    id = vkt_handle_index(300 * 32, 12544, &k);     T(id == 9600 && k == VKH_OK);             /* inside the pool: always an index, never converted */
    id = vkt_handle_index(1000 * 32 + 4, 12544, &k); T(id == 0 && k == VKH_RANGE);            /* past the pool and not a multiple of 32 */
    id = vkt_handle_index(12544 * 32 + 32, 12544, &k); T(id == 0 && k == VKH_RANGE);          /* the offset itself lies past the pool */
    id = vkt_handle_index(5000, 0, &k);             T(id == 5000 && k == VKH_OK);             /* no pool known yet: nothing to check against */
    id = vkt_handle_index(0xfffff, 4096, &k);       T(id == 0 && k == VKH_RANGE);             /* 0xfffff is not a multiple of 32 */
    id = vkt_handle_index(0xfffe0, 40000, &k);      T(id == 32767 && k == VKH_OFFSET);       /* 0xfffe0 / 32 = 32767 lies in a 40000 pool */
    puts(g_fail ? "FAIL" : "ok");
    return g_fail;
}
