#include <rstd/test/gtest.hpp>
import rstd;
import vvk;

TEST(Memory, ModuleOnlyPublicInterface) {
    vvk::DeviceDispatch dispatch {};
    auto                memory_dispatch = vvk::MemoryDispatch::FromDeviceDispatch(dispatch);
    EXPECT_FALSE(memory_dispatch.valid());
    VkMemoryDedicatedRequirements dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    EXPECT_EQ(dedicated.requiresDedicatedAllocation, 0u);
    auto invalid = vvk::MemoryAllocator::Create({});
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(invalid.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
    vvk::AllocatedBuffer buffer;
    EXPECT_FALSE(buffer.valid());
    EXPECT_FALSE(buffer.allocation().valid());
}
