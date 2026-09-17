#include <rstd/test/gtest.hpp>
import rstd;
import vvk;

TEST(Memory, ModuleOnlyPublicInterface) {
    VkImageCreateInfo image { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    image.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatListCreateInfo formats { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    image.pNext = &formats;
    EXPECT_NE(image.flags, 0u);
    vvk::DeviceDispatch dispatch {};
    auto memory_dispatch = vvk::MemoryDispatch::FromDispatch(vvk::InstanceDispatch {}, dispatch);
    EXPECT_FALSE(memory_dispatch.valid());
    VkMemoryDedicatedRequirements dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    EXPECT_EQ(dedicated.requiresDedicatedAllocation, 0u);
    auto invalid = vvk::MemoryAllocator::Create({});
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(invalid.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
    auto upload       = vvk::MemoryRequest::Upload();
    upload.required   = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    upload.preferred  = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    upload.preference = vvk::MemoryPreference::FlagsOnly;
    EXPECT_EQ(upload.host_access, vvk::MemoryHostAccess::SequentialWrite);
    vvk::AllocatedBuffer buffer;
    EXPECT_FALSE(buffer.valid());
    EXPECT_FALSE(buffer.allocation().valid());
}
