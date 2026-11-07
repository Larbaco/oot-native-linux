#include <vulkan/vulkan.h>

__attribute__((constructor)) static void probe_vulkan_loader(void) {
    uint32_t extension_count = 0;
    (void)vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
}
