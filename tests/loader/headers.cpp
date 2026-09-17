#include <vulkan/vulkan.h>
int main() { return VK_API_VERSION_MAJOR(VK_API_VERSION_1_1)==1 ? 0 : 1; }
