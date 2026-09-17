#include <vulkan/vulkan.h>
#include <rstd/test/gtest.hpp>
import rstd;
import vvk;
using namespace rstd::prelude;

namespace
{
struct FakeMemory {
    struct Block {
        bool          live {};
        VkDeviceSize  size {};
        unsigned      type {};
        unsigned      maps {};
        unsigned char data[8192] {};
    } blocks[128];
    struct Buffer {
        bool           live {};
        VkDeviceSize   size {};
        VkDeviceMemory memory {};
    } buffers[128];
    struct Image {
        bool           live {};
        VkDeviceMemory memory {};
    } images[128];
    unsigned next_block { 1 }, next_buffer { 1 }, next_image { 1 };
    unsigned allocations {}, frees {}, creates {}, destroys {}, queries {}, maps {}, unmaps {},
        flushes {}, invalidates {};
    bool fail_create {}, fail_allocate {}, fail_bind {}, fail_map {}, dedicated {},
        prefer_dedicated {}, reject_device {};
    bool                driver_budget {}, device_lost {};
    VkDeviceSize        budget { 8192 };
    VkMappedMemoryRange last_range {};
    VkImageCreateFlags  image_flags {};
    const void*         image_chain {};
}* fake;
template<typename T>
T handle(unsigned id) {
    return vvk::HandleFromIdentity<T>(id);
}
template<typename T>
unsigned index(T value) {
    return unsigned(vvk::OpaqueHandleIdentity(value));
}
VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p                                 = {};
    p->apiVersion                      = VK_API_VERSION_1_1;
    p->limits.nonCoherentAtomSize      = 64;
    p->limits.bufferImageGranularity   = 256;
    p->limits.maxMemoryAllocationCount = 128;
}
VKAPI_ATTR void VKAPI_CALL MemoryProperties(VkPhysicalDevice,
                                            VkPhysicalDeviceMemoryProperties2* p) {
    p->memoryProperties                     = {};
    p->memoryProperties.memoryHeapCount     = 1;
    p->memoryProperties.memoryHeaps[0].size = 1024 * 1024;
    p->memoryProperties.memoryTypeCount     = 2;
    p->memoryProperties.memoryTypes[0]      = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0 };
    p->memoryProperties.memoryTypes[1]      = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    if (p->pNext) {
        auto* b          = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(p->pNext);
        b->heapBudget[0] = fake->driver_budget ? fake->budget : 0;
        b->heapUsage[0]  = 256;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice, const VkMemoryAllocateInfo*         info,
                                        const VkAllocationCallbacks*, VkDeviceMemory* out) {
    if (fake->device_lost) return VK_ERROR_DEVICE_LOST;
    if (fake->fail_allocate || (fake->reject_device && info->memoryTypeIndex == 1))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    EXPECT_LT(info->allocationSize, 8193u);
    auto i               = fake->next_block++;
    fake->blocks[i].live = true;
    fake->blocks[i].size = info->allocationSize;
    fake->blocks[i].type = info->memoryTypeIndex;
    *out                 = handle<VkDeviceMemory>(i);
    ++fake->allocations;
    if (fake->dedicated) {
        EXPECT_NE(info->pNext, nullptr);
        const auto* d = static_cast<const VkMemoryDedicatedAllocateInfo*>(info->pNext);
        EXPECT_TRUE(d->buffer || d->image);
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Free(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    auto& block = fake->blocks[index(memory)];
    EXPECT_TRUE(block.live);
    for (const auto& b : fake->buffers) EXPECT_FALSE(b.live && b.memory == memory);
    for (const auto& i : fake->images) EXPECT_FALSE(i.live && i.memory == memory);
    block.live = false;
    ++fake->frees;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice, const VkBufferCreateInfo*     info,
                                            const VkAllocationCallbacks*, VkBuffer* out) {
    if (fake->fail_create) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto i                = fake->next_buffer++;
    fake->buffers[i].live = true;
    fake->buffers[i].size = info->size;
    *out                  = handle<VkBuffer>(i);
    ++fake->creates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    EXPECT_TRUE(fake->buffers[index(buffer)].live);
    fake->buffers[index(buffer)].live = false;
    ++fake->destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice, const VkImageCreateInfo*     info,
                                           const VkAllocationCallbacks*, VkImage* out) {
    if (fake->fail_create) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    fake->image_flags    = info->flags;
    fake->image_chain    = info->pNext;
    auto i               = fake->next_image++;
    fake->images[i].live = true;
    *out                 = handle<VkImage>(i);
    ++fake->creates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    EXPECT_TRUE(fake->images[index(image)].live);
    fake->images[index(image)].live = false;
    ++fake->destroys;
}
void Requirements(VkMemoryRequirements2* req, VkDeviceSize size) {
    ++fake->queries;
    req->memoryRequirements = { size, 16, 3 };
    auto* dedicated         = static_cast<VkMemoryDedicatedRequirements*>(req->pNext);
    EXPECT_NE(dedicated, nullptr);
    dedicated->requiresDedicatedAllocation = fake->dedicated;
    dedicated->prefersDedicatedAllocation  = fake->prefer_dedicated;
}
VKAPI_ATTR void VKAPI_CALL BufferRequirements(VkDevice, const VkBufferMemoryRequirementsInfo2* info,
                                              VkMemoryRequirements2* req) {
    Requirements(req, fake->buffers[index(info->buffer)].size);
}
VKAPI_ATTR void VKAPI_CALL ImageRequirements(VkDevice, const VkImageMemoryRequirementsInfo2*,
                                             VkMemoryRequirements2* req) {
    Requirements(req, 128);
}
VKAPI_ATTR VkResult VKAPI_CALL BindBuffer(VkDevice, VkBuffer buffer, VkDeviceMemory memory,
                                          VkDeviceSize offset) {
    if (fake->fail_bind) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    EXPECT_EQ(offset % 16, 0u);
    fake->buffers[index(buffer)].memory = memory;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL BindImage(VkDevice, VkImage image, VkDeviceMemory memory,
                                         VkDeviceSize offset) {
    if (fake->fail_bind) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    EXPECT_EQ(offset % 16, 0u);
    fake->images[index(image)].memory = memory;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Map(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
                                   VkDeviceSize, VkMemoryMapFlags, void**        out) {
    if (fake->fail_map) return VK_ERROR_MEMORY_MAP_FAILED;
    auto& b = fake->blocks[index(memory)];
    EXPECT_EQ(b.maps, 0u);
    ++b.maps;
    ++fake->maps;
    *out = b.data + offset;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Unmap(VkDevice, VkDeviceMemory memory) {
    auto& b = fake->blocks[index(memory)];
    EXPECT_EQ(b.maps, 1u);
    --b.maps;
    ++fake->unmaps;
}
VKAPI_ATTR VkResult VKAPI_CALL Flush(VkDevice, unsigned count, const VkMappedMemoryRange* ranges) {
    EXPECT_EQ(count, 1u);
    fake->last_range = *ranges;
    ++fake->flushes;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Invalidate(VkDevice, unsigned count,
                                          const VkMappedMemoryRange* ranges) {
    EXPECT_EQ(count, 1u);
    fake->last_range = *ranges;
    ++fake->invalidates;
    return VK_SUCCESS;
}
auto MakeAllocator(vvk::MemoryMetadata metadata    = alloc::allocator_ref(alloc::GLOBAL),
                   bool                format_list = false) {
    vvk::MemoryDispatch dispatch { Properties,
                                   MemoryProperties,
                                   Allocate,
                                   Free,
                                   CreateBuffer,
                                   DestroyBuffer,
                                   CreateImage,
                                   DestroyImage,
                                   BufferRequirements,
                                   ImageRequirements,
                                   BindBuffer,
                                   BindImage,
                                   Map,
                                   Unmap,
                                   Flush,
                                   Invalidate };
    return vvk::MemoryAllocator::Create(
        { handle<VkPhysicalDevice>(1), handle<VkDevice>(1), 1024, true, dispatch, format_list },
        metadata);
}
auto BufferInfo(VkDeviceSize size = 73) -> VkBufferCreateInfo {
    return {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    };
}
auto ImageInfo() -> VkImageCreateInfo {
    VkImageCreateInfo i { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    i.imageType = VK_IMAGE_TYPE_2D;
    i.format    = VK_FORMAT_R8G8B8A8_UNORM;
    i.extent    = { 4, 4, 1 };
    i.mipLevels = i.arrayLayers = 1;
    i.samples                   = VK_SAMPLE_COUNT_1_BIT;
    i.tiling                    = VK_IMAGE_TILING_OPTIMAL;
    i.usage                     = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return i;
}
} // namespace

struct MemoryFailMetadata {
    int remaining { -1 };
    int live {};
};
namespace rstd
{
template<>
struct Impl<alloc::Allocator, ::MemoryFailMetadata>
    : DefaultInImpl<alloc::Allocator, ::MemoryFailMetadata> {
    auto allocate(alloc::Layout layout) const -> Result<alloc::Allocation, alloc::AllocError> {
        auto& s = const_cast<::MemoryFailMetadata&>(this->self());
        if (s.remaining == 0) return Err(alloc::AllocError {});
        if (s.remaining > 0) --s.remaining;
        auto result = as<alloc::Allocator>(::alloc::GLOBAL).allocate(layout);
        if (result.is_ok()) ++s.live;
        return result;
    }
    void deallocate(void* pointer, alloc::Layout layout) const noexcept {
        --const_cast<::MemoryFailMetadata&>(this->self()).live;
        as<alloc::Allocator>(::alloc::GLOBAL).deallocate(pointer, layout);
    }
};
} // namespace rstd
TEST(Memory, PoolsRequirementsAndDedicated) {
    FakeMemory context;
    fake = &context;
    {
        auto allocator = MakeAllocator().unwrap_unchecked();
        auto a         = allocator.create_buffer(BufferInfo()).unwrap_unchecked();
        auto b         = allocator.create_buffer(BufferInfo()).unwrap_unchecked();
        EXPECT_EQ(fake->queries, 2u);
        EXPECT_EQ(fake->allocations, 1u);
        auto ai = a.allocation().info(), bi = b.allocation().info();
        EXPECT_EQ(ai.memory, bi.memory);
        EXPECT_NE(ai.offset, bi.offset);
        EXPECT_EQ(ai.memory_type, 1u);
        auto image = allocator.create_image(ImageInfo()).unwrap_unchecked();
        EXPECT_NE(image.allocation().info().memory, ai.memory);
        fake->dedicated = true;
        auto separate   = allocator.create_buffer(BufferInfo()).unwrap_unchecked();
        EXPECT_TRUE(separate.allocation().info().dedicated);
        EXPECT_EQ(separate.allocation().info().offset, 0u);
    }
    EXPECT_EQ(fake->allocations, fake->frees);
    EXPECT_EQ(fake->creates, fake->destroys);
}
TEST(Memory, MappingAtomsAndLifetime) {
    FakeMemory context;
    fake                         = &context;
    auto               allocator = MakeAllocator().unwrap_unchecked();
    vvk::MemoryRequest request { .required           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                 .persistent_mapping = true };
    auto               a      = allocator.create_buffer(BufferInfo(), request).unwrap_unchecked();
    auto               b      = allocator.create_buffer(BufferInfo(), request).unwrap_unchecked();
    auto               memory = a.allocation();
    EXPECT_EQ(memory.info().occupied_size, 128u);
    EXPECT_EQ(b.allocation().info().offset % 64, 0u);
    {
        auto m      = memory.map(3, 5).unwrap_unchecked();
        auto second = memory.map().unwrap_unchecked();
        EXPECT_EQ(fake->maps, 1u);
        EXPECT_TRUE(memory.flush(3, 5).is_ok());
        EXPECT_EQ(fake->last_range.offset, 0u);
        EXPECT_EQ(fake->last_range.size, 64u);
        EXPECT_TRUE(b.allocation().invalidate(70, 3).is_ok());
        EXPECT_EQ(fake->last_range.offset, 192u);
        EXPECT_EQ(fake->last_range.size, 64u);
        EXPECT_TRUE(memory.flush(74, 1).is_err());
        EXPECT_TRUE(memory.map(73, 1).is_err());
        allocator.reset();
        a.reset();
        b.reset();
        memory.reset();
        EXPECT_EQ(fake->frees, 0u);
    }
    EXPECT_EQ(fake->frees, 1u);
    EXPECT_EQ(fake->unmaps, 1u);
}
TEST(Memory, VulkanFailuresAndUnsupportedRequests) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        FakeMemory context;
        fake = &context;
        {
            auto allocator      = MakeAllocator().unwrap_unchecked();
            fake->fail_create   = mode == 0;
            fake->fail_allocate = mode == 1;
            fake->fail_bind     = mode == 2;
            fake->fail_map      = mode == 3;
            fake->device_lost   = mode == 4;
            auto result         = allocator.create_buffer(
                BufferInfo(),
                { .required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, .persistent_mapping = true });
            EXPECT_TRUE(result.is_err());
            if (mode == 4)
                EXPECT_EQ(result.unwrap_err_unchecked().api_result, VK_ERROR_DEVICE_LOST);
            EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 0u);
            allocator.trim();
        }
        EXPECT_EQ(fake->allocations, fake->frees);
        EXPECT_EQ(fake->creates, fake->destroys);
    }
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto info      = BufferInfo();
    info.flags     = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    EXPECT_TRUE(allocator.create_buffer(info).is_err());
    info       = BufferInfo();
    info.pNext = &info;
    EXPECT_TRUE(allocator.create_buffer(info).is_err());
    EXPECT_EQ(fake->creates, 0u);
}
TEST(Memory, HostMetadataFailureRollback) {
    bool succeeded = false;
    for (int fail = 0; fail < 20; ++fail) {
        FakeMemory context;
        fake = &context;
        MemoryFailMetadata metadata { fail };
        {
            auto allocator_result = MakeAllocator(alloc::allocator_ref(metadata));
            if (allocator_result.is_ok()) {
                auto allocator = allocator_result.unwrap_unchecked();
                auto result    = allocator.create_buffer(BufferInfo());
                if (result.is_ok())
                    succeeded = true;
                else
                    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 0u);
            }
        }
        EXPECT_EQ(metadata.live, 0);
        EXPECT_EQ(fake->allocations, fake->frees);
        EXPECT_EQ(fake->creates, fake->destroys);
    }
    EXPECT_TRUE(succeeded);
}
TEST(Memory, BudgetFallbackAndMemoryTypeFallback) {
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator().unwrap_unchecked();
    EXPECT_EQ(allocator.budget().heaps[0].source, vvk::MemoryBudgetSource::AllocatorEstimate);
    fake->driver_budget = true;
    fake->budget        = 500;
    EXPECT_EQ(allocator.budget().heaps[0].source, vvk::MemoryBudgetSource::Driver);
    EXPECT_TRUE(allocator.create_buffer(BufferInfo(), { .within_budget = true }).is_err());
    fake->budget        = 8192;
    fake->reject_device = true;
    auto buffer         = allocator.create_buffer(BufferInfo()).unwrap_unchecked();
    EXPECT_EQ(buffer.allocation().info().memory_type, 0u);
    buffer.reset();
    allocator.trim();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 0u);
}
TEST(Memory, SubmissionLeaseNeedsEveryCompletionSource) {
    FakeMemory context;
    fake                  = &context;
    auto allocator        = MakeAllocator().unwrap_unchecked();
    auto buffer           = allocator.create_buffer(BufferInfo()).unwrap_unchecked();
    auto submission_lease = buffer.clone();
    buffer.reset();
    auto queue =
        vvk::MakeQueueDomain(handle<VkDevice>(1), handle<VkQueue>(1), u32(0), u32(0), u64(1));
    auto source  = vvk::MakeTimelineCompletionSource(queue, handle<VkSemaphore>(1), u64(1));
    auto source2 = vvk::MakeTimelineCompletionSource(queue, handle<VkSemaphore>(2), u64(1));
    vvk::SubmissionToken     a { source, u64(3) }, b { source2, u64(7) };
    vvk::CompletedWatermarks completed;
    completed.Observe({ source, u64(100) });
    EXPECT_TRUE(completed.Covers(a));
    EXPECT_FALSE(completed.Covers(b));
    auto stale       = source2;
    stale.generation = u64(2);
    completed.Observe({ stale, u64(100) });
    EXPECT_FALSE(completed.Covers(b));
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 1u);
    vvk::TimelineCompletionObserver observer(
        handle<VkDevice>(1),
        +[](VkDevice, VkSemaphore, rstd::uint64_t*) -> VkResult {
            return VK_ERROR_DEVICE_LOST;
        },
        +[](VkDevice, VkSemaphore, rstd::uint64_t, rstd::uint64_t) -> VkResult {
            return VK_ERROR_DEVICE_LOST;
        });
    auto observation = observer.Poll(source2);
    EXPECT_FALSE(observation.observed());
    EXPECT_EQ(observation.status, vvk::CompletionObservationStatus::DeviceLost);
    completed.Observe(b);
    if (completed.Covers(a) && completed.Covers(b)) submission_lease.reset();
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 0u);
}
TEST(Memory, EmptyBlockRetentionAndDedicatedTail) {
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto a         = allocator.create_buffer(BufferInfo(400)).unwrap_unchecked();
    auto b         = allocator.create_buffer(BufferInfo(400)).unwrap_unchecked();
    auto c         = allocator.create_buffer(BufferInfo(400)).unwrap_unchecked();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 2u);
    a.reset();
    b.reset();
    c.reset();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 1u);
    allocator.trim();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 0u);
    fake->prefer_dedicated = true;
    auto tail              = allocator
                                 .create_buffer(BufferInfo(73),
                                                { .required           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                  .persistent_mapping = true })
                                 .unwrap_unchecked();
    auto memory            = tail.allocation();
    EXPECT_TRUE(memory.info().dedicated);
    EXPECT_TRUE(memory.flush(70, 3).is_ok());
    EXPECT_EQ(fake->last_range.offset, 64u);
    EXPECT_EQ(fake->last_range.size, 9u);
    memory.reset();
    tail.reset();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 0u);
    fake->prefer_dedicated = false;
    fake->fail_bind        = true;
    EXPECT_TRUE(allocator.create_image(ImageInfo()).is_err());
    allocator.trim();
    EXPECT_EQ(fake->allocations, fake->frees);
    EXPECT_EQ(fake->creates, fake->destroys);
    auto unsupported  = ImageInfo();
    unsupported.flags = VK_IMAGE_CREATE_DISJOINT_BIT;
    EXPECT_TRUE(allocator.create_image(unsupported).is_err());
}

TEST(Memory, RingSubmissionCompletionAndAtomIsolation) {
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto upload    = allocator
                         .create_buffer(BufferInfo(256),
                                        { .required           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                          .persistent_mapping = true })
                         .unwrap_unchecked();
    auto memory    = upload.allocation();
    auto mapping   = memory.map().unwrap_unchecked();
    alloc::RingRangeAllocator ranges(256);
    auto                      first  = ranges.allocate(128, 64).unwrap_unchecked();
    auto                      second = ranges.allocate(128, 64).unwrap_unchecked();
    auto*                     bytes  = static_cast<unsigned char*>(mapping.data());
    for (unsigned i = 0; i < 73; ++i) {
        bytes[first.offset + i]  = 0x35;
        bytes[second.offset + i] = 0x97;
    }
    EXPECT_TRUE(memory.flush(first.offset, 73).is_ok());
    EXPECT_EQ(fake->last_range.offset, 0u);
    EXPECT_EQ(fake->last_range.size, 128u);
    EXPECT_TRUE(memory.flush(second.offset, 73).is_ok());
    EXPECT_EQ(fake->last_range.offset, 128u);
    EXPECT_EQ(fake->last_range.size, 128u);
    auto queue =
        vvk::MakeQueueDomain(handle<VkDevice>(1), handle<VkQueue>(1), u32(0), u32(0), u64(1));
    auto source_a = vvk::MakeTimelineCompletionSource(queue, handle<VkSemaphore>(1), u64(1));
    auto source_b = vvk::MakeTimelineCompletionSource(queue, handle<VkSemaphore>(2), u64(1));
    struct Pending {
        alloc::RingRangeAllocation range;
        vvk::SubmissionToken       requirements[2];
        unsigned                   count;
        vvk::AllocatedBuffer       lease;
    };
    alloc::vec::Vec<Pending> pending;
    pending.push(
        Pending { first, { { source_a, u64(3) }, { source_b, u64(5) } }, 2, upload.clone() });
    pending.push(Pending { second, { { source_b, u64(7) }, {} }, 1, upload.clone() });
    upload.reset();
    vvk::CompletedWatermarks completed;
    auto                     collect = [&] {
        while (! pending.is_empty()) {
            const auto& first_pending = pending[usize(0)];
            bool        ready         = true;
            for (unsigned i = 0; i < first_pending.count; ++i)
                ready &= completed.Covers(first_pending.requirements[i]);
            if (! ready) break;
            auto retired = pending.remove(usize(0));
            EXPECT_TRUE(ranges.release(retired.range.id).is_ok());
        }
    };
    completed.Observe({ source_b, u64(7) });
    collect();
    EXPECT_EQ(pending.len(), usize(2));
    EXPECT_EQ(ranges.statistics().free_bytes, 0u);
    EXPECT_EQ(ranges.release(second.id).unwrap_err_unchecked(), alloc::RangeError::OutOfOrder);
    EXPECT_TRUE(ranges.allocate(1, 1).is_err());
    EXPECT_EQ(fake->destroys, 0u);
    auto stale       = source_a;
    stale.generation = u64(2);
    completed.Observe({ stale, u64(100) });
    collect();
    EXPECT_EQ(pending.len(), usize(2));
    vvk::TimelineCompletionObserver lost(
        handle<VkDevice>(1),
        +[](VkDevice, VkSemaphore, rstd::uint64_t*) -> VkResult {
            return VK_ERROR_DEVICE_LOST;
        },
        +[](VkDevice, VkSemaphore, rstd::uint64_t, rstd::uint64_t) -> VkResult {
            return VK_ERROR_DEVICE_LOST;
        });
    auto observation = lost.Poll(source_a);
    EXPECT_EQ(observation.status, vvk::CompletionObservationStatus::DeviceLost);
    if (observation.observed()) completed.Observe(observation.completed);
    collect();
    EXPECT_EQ(pending.len(), usize(2));
    EXPECT_EQ(fake->destroys, 0u);
    for (unsigned i = 0; i < 73; ++i) {
        EXPECT_EQ(bytes[first.offset + i], 0x35u);
        EXPECT_EQ(bytes[second.offset + i], 0x97u);
    }
    completed.Observe({ source_a, u64(3) });
    collect();
    EXPECT_TRUE(pending.is_empty());
    EXPECT_EQ(fake->destroys, 1u);
    EXPECT_EQ(ranges.statistics().free_bytes, 256u);
    auto reused = ranges.allocate(256, 64).unwrap_unchecked();
    EXPECT_EQ(reused.offset, 0u);
    EXPECT_TRUE(ranges.release(first.id).is_err());
    EXPECT_TRUE(ranges.release(reused.id).is_ok());
}

TEST(Memory, ImageConstraintsAndBorrowedChain) {
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), true).unwrap_unchecked();
    VkFormat                    formats[] { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
    VkImageFormatListCreateInfo list {
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats
    };
    auto info        = ImageInfo();
    info.flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.arrayLayers = 6;
    info.pNext       = &list;
    {
        auto image = allocator.create_image(info);
        ASSERT_TRUE(image.is_ok());
        EXPECT_EQ(fake->image_flags, info.flags);
        EXPECT_EQ(fake->image_chain, &list);
        EXPECT_EQ(fake->queries, 1u);
    }
    auto reject = [&](vvk::MemoryErrorKind kind) {
        auto result = allocator.create_image(info);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err_unchecked().kind, kind);
        EXPECT_EQ(fake->creates, 1u);
        EXPECT_EQ(fake->queries, 1u);
    };
    list.pNext = &list;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    VkImageFormatListCreateInfo duplicate = list;
    duplicate.pNext                       = nullptr;
    list.pNext                            = &duplicate;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    VkExternalMemoryImageCreateInfo external {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO
    };
    list.pNext = &external;
    reject(vvk::MemoryErrorKind::Unsupported);
    list.pNext        = nullptr;
    list.pViewFormats = nullptr;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    list.pViewFormats = formats;
    formats[1]        = VK_FORMAT_UNDEFINED;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.extent.height = 2;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    info.extent.height = 4;
    info.arrayLayers   = 5;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    info.arrayLayers = 6;
    info.imageType   = VK_IMAGE_TYPE_3D;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.samples   = VK_SAMPLE_COUNT_4_BIT;
    reject(vvk::MemoryErrorKind::InvalidRequest);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    const VkImageCreateFlags unsupported_flags[] { VK_IMAGE_CREATE_SPARSE_BINDING_BIT,
                                                   VK_IMAGE_CREATE_DISJOINT_BIT,
                                                   VK_IMAGE_CREATE_PROTECTED_BIT,
                                                   VK_IMAGE_CREATE_ALIAS_BIT };
    for (auto flag : unsupported_flags) {
        info.flags = flag;
        reject(vvk::MemoryErrorKind::Unsupported);
    }
    info.flags              = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    auto without_capability = MakeAllocator().unwrap_unchecked();
    auto rejected           = without_capability.create_image(info);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::Unsupported);
    info.flags           = 0;
    list.viewFormatCount = 0;
    list.pViewFormats    = nullptr;
    EXPECT_TRUE(allocator.create_image(info).is_ok());
    list.viewFormatCount = 1;
    list.pViewFormats    = formats;
    EXPECT_TRUE(allocator.create_image(info).is_ok());
    info.pNext = nullptr;
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    EXPECT_TRUE(without_capability.create_image(info).is_ok());
}

TEST(Memory, CompatibleImageFailureRollback) {
    bool succeeded = false;
    for (int failure = 0; failure < 24; ++failure) {
        FakeMemory context;
        fake = &context;
        MemoryFailMetadata metadata { failure < 4 ? -1 : failure - 4 };
        {
            auto made = MakeAllocator(alloc::allocator_ref(metadata), true);
            if (made.is_ok()) {
                auto allocator = made.unwrap_unchecked();
                auto info      = ImageInfo();
                info.flags =
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                info.arrayLayers                   = 6;
                VkFormat                    format = info.format;
                VkImageFormatListCreateInfo list {
                    VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 1, &format
                };
                info.pNext          = &list;
                fake->fail_create   = failure == 0;
                fake->fail_allocate = failure == 1;
                fake->fail_bind     = failure == 2;
                fake->device_lost   = failure == 3;
                {
                    auto image = allocator.create_image(info);
                    succeeded |= image.is_ok();
                    if (failure < 4) {
                        ASSERT_TRUE(image.is_err());
                    }
                    if (failure == 3)
                        EXPECT_EQ(image.unwrap_err_unchecked().api_result, VK_ERROR_DEVICE_LOST);
                    EXPECT_EQ(fake->queries, fake->creates);
                }
                EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 0u);
            }
        }
        EXPECT_EQ(metadata.live, 0);
        EXPECT_EQ(fake->creates, fake->destroys);
        EXPECT_EQ(fake->allocations, fake->frees);
    }
    EXPECT_TRUE(succeeded);
}
