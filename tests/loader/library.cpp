#include <vulkan/vulkan.h>
#include <cstring>
#ifdef VVK_MISSING_ROOT
extern "C" int vvk_test_without_root() { return 1; }
#else
namespace {
void (*observer)(int) {};
void Event(int value) { if(observer)observer(value); }
void SetObserver(void (*callback)(int)) { observer=callback; }
__attribute__((destructor)) void Closed() { Event(5); }
void VKAPI_CALL Uncalled() {}
VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance* out) {
    *out=reinterpret_cast<VkInstance>(1);Event(1);return VK_SUCCESS;
}
void VKAPI_CALL DestroyInstance(VkInstance,const VkAllocationCallbacks*) { Event(4); }
VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice* out) {
    *out=reinterpret_cast<VkDevice>(2);Event(2);return VK_SUCCESS;
}
void VKAPI_CALL DestroyDevice(VkDevice,const VkAllocationCallbacks*) { Event(3); }
void VKAPI_CALL Properties(VkPhysicalDevice,VkPhysicalDeviceProperties* out) { out->apiVersion=VK_API_VERSION_1_3; }
PFN_vkVoidFunction VKAPI_CALL DeviceResolver(VkDevice,const char* name) {
    if(std::strcmp(name,"vkDestroyDevice")==0)return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    return Uncalled;
}
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance,const char* name) {
    if(std::strcmp(name,"vvkTestSetObserver")==0)return reinterpret_cast<PFN_vkVoidFunction>(SetObserver);
    if(std::strcmp(name,"vkCreateInstance")==0)return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if(std::strcmp(name,"vkDestroyInstance")==0)return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    if(std::strcmp(name,"vkCreateDevice")==0)return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    if(std::strcmp(name,"vkGetPhysicalDeviceProperties")==0)return reinterpret_cast<PFN_vkVoidFunction>(Properties);
    if(std::strcmp(name,"vkGetDeviceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(DeviceResolver);
    return Uncalled;
}
#endif
