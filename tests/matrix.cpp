#include <vulkan/vk_platform.h>
#include <rstd/test/gtest.hpp>
import rstd;
import vvk;
using namespace rstd::prelude;
namespace
{
bool                missing_query = false;
unsigned            queries, fills;
VkResult            failure = VK_SUCCESS;
VkResult VKAPI_CALL Matrices(VkPhysicalDevice, uint32_t* count,
                             VkCooperativeMatrixPropertiesKHR* output) {
    if (failure != VK_SUCCESS) return failure;
    if (! output) {
        *count = ++queries == 1 ? 1 : 3;
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < *count; ++i) {
        EXPECT_EQ(output[i].sType, VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR);
        EXPECT_EQ(output[i].pNext, nullptr);
    }
    if (++fills == 1) return VK_INCOMPLETE;
    output[0].MSize = 16;
    output[1].MSize = 32;
    *count          = 2;
    return VK_SUCCESS;
}
void VKAPI_CALL               Uncalled() {}
PFN_vkVoidFunction VKAPI_CALL Resolver(VkInstance, const char* name) {
    if (missing_query &&
        rstd::ffi::CStr::from_ptr(name).to_bytes() ==
            rstd::ffi::CStr::from_ptr("vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR")
                .to_bytes())
        return nullptr;
    return Uncalled;
}
} // namespace
TEST(Matrix, EnumerationRetriesAndClearsFailures) {
    vvk::InstanceDispatch dispatch;
    dispatch.vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR = Matrices;
    vvk::PhysicalDevice physical(reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)), dispatch);
    rstd::vec::Vec<VkCooperativeMatrixPropertiesKHR> output;
    queries = fills = 0;
    failure         = VK_SUCCESS;
    ASSERT_EQ(physical.GetCooperativeMatrixPropertiesKHR(output), VK_SUCCESS);
    EXPECT_EQ(queries, 2u);
    EXPECT_EQ(fills, 2u);
    ASSERT_EQ(output.len().to_primitive(), 2u);
    EXPECT_EQ(output[usize(0)].MSize, 16u);
    EXPECT_EQ(output[usize(1)].MSize, 32u);
    failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    EXPECT_EQ(physical.GetCooperativeMatrixPropertiesKHR(output), failure);
    EXPECT_TRUE(output.is_empty());
    dispatch.vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR = nullptr;
    EXPECT_EQ(physical.GetCooperativeMatrixPropertiesKHR(output), VK_ERROR_EXTENSION_NOT_PRESENT);
    EXPECT_TRUE(output.is_empty());
}
TEST(Matrix, OptionalInstanceQuery) {
    vvk::GlobalDispatch global;
    global.vkGetInstanceProcAddr = Resolver;
    missing_query                = false;
    auto loaded = vvk::LoadInstance(global, reinterpret_cast<VkInstance>(uintptr_t(1)), {});
    ASSERT_TRUE(loaded.is_ok());
    EXPECT_NE(loaded.unwrap_unchecked().vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR, nullptr);
    missing_query = true;
    auto absent   = vvk::LoadInstance(global, reinterpret_cast<VkInstance>(uintptr_t(1)), {});
    ASSERT_TRUE(absent.is_ok());
    EXPECT_EQ(absent.unwrap_unchecked().vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR, nullptr);
}
