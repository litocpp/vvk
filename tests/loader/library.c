#include <vulkan/vulkan.h>
#include <string.h>
#ifdef VVK_MISSING_ROOT
int vvk_test_without_root(void) { return 1; }
#else

static void (*observer)(int);
static void Event(int value) {
    if (observer) observer(value);
}
static void                             SetObserver(void (*callback)(int)) { observer = callback; }
__attribute__((destructor)) static void Closed(void) { Event(5); }
static void VKAPI_CALL                  Uncalled(void) {}
static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*  info,
                                          const VkAllocationCallbacks* callbacks, VkInstance* out) {
    *out = (VkInstance)1;
    Event(1);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyInstance(VkInstance                   instance,
                                       const VkAllocationCallbacks* callbacks) {
    Event(4);
}
static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                        const VkAllocationCallbacks* callbacks, VkDevice* out) {
    *out = (VkDevice)2;
    Event(2);
    return VK_SUCCESS;
}
static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* callbacks) {
    Event(3);
}
static void VKAPI_CALL Properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
    out->apiVersion = VK_API_VERSION_1_3;
}
static PFN_vkVoidFunction VKAPI_CALL DeviceResolver(VkDevice device, const char* name) {
    if (strcmp(name, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)DestroyDevice;
    return Uncalled;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance  instance,
                                                               const char* name) {
    if (strcmp(name, "vvkTestSetObserver") == 0) return (PFN_vkVoidFunction)SetObserver;
    if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)DestroyInstance;
    if (strcmp(name, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)CreateDevice;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0) return (PFN_vkVoidFunction)Properties;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)DeviceResolver;
    return Uncalled;
}
#endif
