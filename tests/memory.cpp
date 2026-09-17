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
    bool         driver_budget {}, device_lost {};
    VkDeviceSize budget { 8192 };
    VkDeviceSize heap_size { 1024 * 1024 }, max_allocation_size { 8192 }, max_success_size { 8192 };
    VkDeviceSize driver_usage { 256 }, atom { 64 }, requirement_size {};
    unsigned     max_allocations { 128 }, budget_queries {};
    bool         reject_shared {}, reject_dedicated {};
    VkResult     allocation_error { VK_SUCCESS };
    struct Attempt {
        VkDeviceSize size;
        unsigned     type;
        bool         dedicated;
    } attempts[256] {};
    VkPhysicalDeviceMemoryProperties topology {};
    unsigned                         requirement_types { 3 }, reject_types {};
    unsigned                         attempt_count {};
    VkMappedMemoryRange              last_range {};
    VkImageCreateFlags               image_flags {};
    const void*                      image_chain {};
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
    p->limits.nonCoherentAtomSize      = fake->atom;
    p->limits.bufferImageGranularity   = 256;
    p->limits.maxMemoryAllocationCount = fake->max_allocations;
}
VKAPI_ATTR void VKAPI_CALL Properties2(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2* p) {
    Properties(gpu, &p->properties);
    auto* limits = static_cast<VkPhysicalDeviceMaintenance3Properties*>(p->pNext);
    EXPECT_EQ(limits->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES);
    limits->maxMemoryAllocationSize = fake->max_allocation_size;
}
VKAPI_ATTR void VKAPI_CALL MemoryProperties(VkPhysicalDevice,
                                            VkPhysicalDeviceMemoryProperties2* p) {
    p->memoryProperties                     = {};
    p->memoryProperties.memoryHeapCount     = 1;
    p->memoryProperties.memoryHeaps[0].size = fake->heap_size;
    p->memoryProperties.memoryTypeCount     = 2;
    p->memoryProperties.memoryTypes[0]      = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0 };
    p->memoryProperties.memoryTypes[1]      = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    if (fake->topology.memoryTypeCount) p->memoryProperties = fake->topology;
    if (p->pNext) {
        ++fake->budget_queries;
        auto* b          = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(p->pNext);
        b->heapBudget[0] = fake->driver_budget ? fake->budget : 0;
        b->heapUsage[0]  = fake->driver_usage;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice, const VkMemoryAllocateInfo*         info,
                                        const VkAllocationCallbacks*, VkDeviceMemory* out) {
    const bool separate                   = info->pNext != nullptr;
    fake->attempts[fake->attempt_count++] = { info->allocationSize,
                                              info->memoryTypeIndex,
                                              separate };
    if (fake->reject_types & (1u << info->memoryTypeIndex)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (fake->allocation_error != VK_SUCCESS) return fake->allocation_error;
    if ((separate && fake->reject_dedicated) || (! separate && fake->reject_shared) ||
        info->allocationSize > fake->max_success_size)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (fake->device_lost) return VK_ERROR_DEVICE_LOST;
    if (fake->fail_allocate || (fake->reject_device && info->memoryTypeIndex == 1))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;

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
    req->memoryRequirements = { size, 16, fake->requirement_types };
    auto* dedicated         = static_cast<VkMemoryDedicatedRequirements*>(req->pNext);
    EXPECT_NE(dedicated, nullptr);
    dedicated->requiresDedicatedAllocation = fake->dedicated;
    dedicated->prefersDedicatedAllocation  = fake->prefer_dedicated;
}
VKAPI_ATTR void VKAPI_CALL BufferRequirements(VkDevice, const VkBufferMemoryRequirementsInfo2* info,
                                              VkMemoryRequirements2* req) {
    Requirements(req,
                 fake->requirement_size ? fake->requirement_size
                                        : fake->buffers[index(info->buffer)].size);
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
    if (b.size > sizeof(b.data)) return VK_ERROR_MEMORY_MAP_FAILED;
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
auto MakeAllocator(vvk::MemoryMetadata    metadata    = alloc::allocator_ref(alloc::GLOBAL),
                   bool                   format_list = false,
                   vvk::MemoryBlockPolicy policy      = { 1024, 1024, 0, false }) {
    vvk::MemoryDispatch            dispatch { Properties,
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
                                              Invalidate,
                                              Properties2 };
    vvk::MemoryAllocatorCreateInfo info {
        handle<VkPhysicalDevice>(1), handle<VkDevice>(1), policy, true, dispatch, format_list
    };
    return vvk::MemoryAllocator::Create(info, metadata);
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

TEST(Memory, GrowingBlocksAndTrim) {
    FakeMemory context;
    fake = &context;
    auto allocator =
        MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 256, 1024, 3, true })
            .unwrap_unchecked();
    vvk::AllocatedBuffer buffers[10];
    for (auto& buffer : buffers) {
        auto result = allocator.create_buffer(BufferInfo(256));
        ASSERT_TRUE(result.is_ok());
        buffer = result.unwrap_unchecked();
    }
    ASSERT_EQ(fake->attempt_count, 4u);
    EXPECT_EQ(fake->attempts[0].size, 256u);
    EXPECT_EQ(fake->attempts[1].size, 512u);
    EXPECT_EQ(fake->attempts[2].size, 1024u);
    EXPECT_EQ(fake->attempts[3].size, 1024u);
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 10u);
    for (auto& buffer : buffers) buffer.reset();
    EXPECT_EQ(allocator.budget().heaps[0].block_count, 1u);
    allocator.trim();
    EXPECT_EQ(fake->allocations, fake->frees);
    auto fresh = allocator.create_buffer(BufferInfo(256));
    ASSERT_TRUE(fresh.is_ok());
    EXPECT_EQ(fake->attempts[4].size, 256u);
}

TEST(Memory, ShrinkAndDedicatedFallbackOrder) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        FakeMemory context;
        fake                   = &context;
        fake->max_success_size = mode == 0 ? 256 : 8192;
        fake->reject_shared    = mode == 1;
        fake->reject_dedicated = mode >= 2;
        fake->prefer_dedicated = mode == 2;
        const auto policy      = vvk::MemoryBlockPolicy { 1024, 1024, mode == 0 ? 3u : 0u, true };
        auto       allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, policy).unwrap_unchecked();
        auto result =
            allocator.create_buffer(BufferInfo(mode == 3 ? 600 : 128), { .dedicated = mode == 4 });
        if (mode == 4) {
            ASSERT_TRUE(result.is_err());
            EXPECT_EQ(fake->attempt_count, 2u);
            EXPECT_TRUE(fake->attempts[0].dedicated);
            EXPECT_TRUE(fake->attempts[1].dedicated);
            EXPECT_NE(fake->attempts[0].type, fake->attempts[1].type);
        } else {
            ASSERT_TRUE(result.is_ok());
            auto buffer = result.unwrap_unchecked();
            EXPECT_EQ(buffer.allocation().info().memory_type, 1u);
            EXPECT_EQ(buffer.allocation().info().dedicated, mode == 1);
            if (mode == 0) {
                ASSERT_EQ(fake->attempt_count, 3u);
                EXPECT_EQ(fake->attempts[0].size, 1024u);
                EXPECT_EQ(fake->attempts[1].size, 512u);
                EXPECT_EQ(fake->attempts[2].size, 256u);
            } else {
                ASSERT_EQ(fake->attempt_count, 2u);
                EXPECT_EQ(fake->attempts[0].dedicated, mode != 1);
                EXPECT_EQ(fake->attempts[1].dedicated, mode == 1);
                EXPECT_EQ(fake->attempts[0].type, fake->attempts[1].type);
            }
        }
        EXPECT_EQ(fake->queries, 1u);
    }
    FakeMemory context;
    fake                   = &context;
    fake->dedicated        = true;
    fake->reject_dedicated = true;
    auto allocator =
        MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 256, 1024, 3, true })
            .unwrap_unchecked();
    auto result = allocator.create_image(ImageInfo());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(fake->attempt_count, 2u);
    EXPECT_TRUE(fake->attempts[0].dedicated);
    EXPECT_TRUE(fake->attempts[1].dedicated);
}

TEST(Memory, BudgetChargesPhysicalBlocksAndRefreshes) {
    FakeMemory context;
    fake                = &context;
    fake->driver_budget = true;
    fake->budget        = 600;
    fake->driver_usage  = 256;
    auto allocator =
        MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1024, 1024, 3, true })
            .unwrap_unchecked();
    auto first = allocator.create_buffer(BufferInfo(128), { .within_budget = true });
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(fake->attempt_count, 1u);
    EXPECT_EQ(fake->attempts[0].size, 256u);
    EXPECT_EQ(fake->budget_queries, 3u);
    fake->driver_usage = 700;
    auto second        = allocator.create_buffer(BufferInfo(128), { .within_budget = true });
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(fake->attempt_count, 1u);
    EXPECT_EQ(fake->budget_queries, 3u);
    auto rejected = allocator.create_buffer(BufferInfo(128), { .within_budget = true });
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(fake->attempt_count, 1u);
    EXPECT_GT(fake->budget_queries, 3u);
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 2u);
}

TEST(Memory, AllocationErrorsDoNotRetry) {
    const VkResult errors[] { VK_ERROR_OUT_OF_HOST_MEMORY,
                              VK_ERROR_DEVICE_LOST,
                              VK_ERROR_TOO_MANY_OBJECTS,
                              VK_ERROR_UNKNOWN };
    for (auto error : errors) {
        FakeMemory context;
        fake                   = &context;
        fake->allocation_error = error;
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1024, 1024, 3, true })
                .unwrap_unchecked();
        auto result = allocator.create_buffer(BufferInfo(128));
        ASSERT_TRUE(result.is_err());
        const auto detail = result.unwrap_err_unchecked();
        EXPECT_EQ(detail.api_result, error);
        EXPECT_EQ(detail.device_allocation_attempts, 1u);
        EXPECT_EQ(detail.memory_type, 1u);
        EXPECT_EQ(detail.allocation_size, 1024u);
        EXPECT_FALSE(detail.dedicated);
        EXPECT_EQ(fake->attempt_count, 1u);
        EXPECT_EQ(fake->creates, fake->destroys);
    }
}

TEST(Memory, PolicyLimitsAndExhaustion) {
    FakeMemory context;
    fake = &context;
    const vvk::MemoryBlockPolicy invalid[] {
        { 0, 0 }, { 0, 1024 }, { 1024, 0 }, { 1024, 256 }, { 1, 1, 64 }
    };
    for (auto policy : invalid) {
        auto result = MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, policy);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
    }
    fake->max_allocation_size = 256;
    fake->heap_size           = 512;
    auto allocator =
        MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1024, 2048, 3, false })
            .unwrap_unchecked();
    auto result = allocator.create_buffer(BufferInfo(128));
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(fake->attempt_count, 1u);
    EXPECT_EQ(fake->attempts[0].size, 256u);
    fake->fail_allocate = true;
    auto failed         = allocator.create_buffer(BufferInfo(256));
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(failed.unwrap_err_unchecked().device_allocation_attempts, 2u);
    EXPECT_EQ(fake->attempt_count, 3u);
    EXPECT_EQ(fake->attempts[1].type, 1u);
    EXPECT_EQ(fake->attempts[2].type, 0u);
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 1u);
}

TEST(Memory, GrowthSaturatesAndAtomOverflowStops) {
    {
        FakeMemory context;
        fake            = &context;
        fake->heap_size = fake->max_allocation_size = fake->max_success_size = ~VkDeviceSize(0);
        const auto initial     = VkDeviceSize(1) << 63;
        auto       allocator   = MakeAllocator(alloc::allocator_ref(alloc::GLOBAL),
                                               false,
                                               { initial, ~VkDeviceSize(0), 0, false })
                                     .unwrap_unchecked();
        fake->requirement_size = initial - 1;
        auto large             = allocator.create_buffer(BufferInfo());
        ASSERT_TRUE(large.is_ok());
        fake->requirement_size = 0;
        fake->allocation_error = VK_ERROR_DEVICE_LOST;
        auto failed            = allocator.create_buffer(BufferInfo());
        ASSERT_TRUE(failed.is_err());
        ASSERT_EQ(fake->attempt_count, 2u);
        EXPECT_EQ(fake->attempts[0].size, initial);
        EXPECT_EQ(fake->attempts[1].size, ~VkDeviceSize(0));
        EXPECT_EQ(failed.unwrap_err_unchecked().api_result, VK_ERROR_DEVICE_LOST);
    }
    {
        FakeMemory context;
        fake                   = &context;
        fake->requirement_size = ~VkDeviceSize(0);
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 256, 1024, 3, true })
                .unwrap_unchecked();
        auto result = allocator.create_buffer(BufferInfo(),
                                              { .required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT });
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
        EXPECT_EQ(fake->attempt_count, 0u);
    }
}

TEST(Memory, AllocationCountAndEstimatedBudget) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        FakeMemory context;
        fake                  = &context;
        fake->max_allocations = mode == 0 ? 1 : 128;
        fake->heap_size       = 512;
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 512, 512, 0, false })
                .unwrap_unchecked();
        auto first = allocator.create_buffer(BufferInfo(256), { .within_budget = true });
        ASSERT_TRUE(first.is_ok());
        auto second = allocator.create_buffer(BufferInfo(256), { .within_budget = true });
        ASSERT_TRUE(second.is_ok());
        auto failed = allocator.create_buffer(BufferInfo(256), { .within_budget = true });
        ASSERT_TRUE(failed.is_err());
        EXPECT_EQ(fake->attempt_count, 1u);
        EXPECT_EQ(failed.unwrap_err_unchecked().device_allocation_attempts, 0u);
        EXPECT_EQ(failed.unwrap_err_unchecked().api_result,
                  mode == 0 ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_DEVICE_MEMORY);
        auto heap = allocator.budget().heaps[0];
        EXPECT_EQ(heap.source, vvk::MemoryBudgetSource::AllocatorEstimate);
        EXPECT_EQ(heap.block_bytes, 512u);
        EXPECT_EQ(heap.allocation_count, 2u);
    }
}

TEST(Memory, PolicyMetadataMappingAndBindingRollback) {
    bool succeeded = false;
    for (int failure = 0; failure < 24; ++failure) {
        FakeMemory context;
        fake = &context;
        MemoryFailMetadata metadata { failure < 2 ? -1 : failure - 2 };
        {
            auto made =
                MakeAllocator(alloc::allocator_ref(metadata), false, { 256, 1024, 3, true });
            if (made.is_ok()) {
                auto allocator  = made.unwrap_unchecked();
                fake->fail_bind = failure == 0;
                fake->fail_map  = failure == 1;
                {
                    auto result =
                        allocator.create_buffer(BufferInfo(73), { .persistent_mapping = true });
                    succeeded |= result.is_ok();
                    if (failure < 2) {
                        EXPECT_TRUE(result.is_err());
                    }
                    EXPECT_LE(fake->attempt_count, 1u);
                }
                EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 0u);
            }
        }
        EXPECT_EQ(metadata.live, 0);
        EXPECT_EQ(fake->creates, fake->destroys);
        EXPECT_EQ(fake->allocations, fake->frees);
        EXPECT_EQ(fake->maps, fake->unmaps);
    }
    EXPECT_TRUE(succeeded);
}

TEST(Memory, ShrinkBoundsAndAtomPadding) {
    {
        FakeMemory context;
        fake                   = &context;
        fake->max_success_size = 128;
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1000, 1000, 63, false })
                .unwrap_unchecked();
        auto result = allocator.create_buffer(BufferInfo(73),
                                              { .required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT });
        ASSERT_TRUE(result.is_ok());
        ASSERT_EQ(fake->attempt_count, 4u);
        EXPECT_EQ(fake->attempts[0].size, 1000u);
        EXPECT_EQ(fake->attempts[1].size, 500u);
        EXPECT_EQ(fake->attempts[2].size, 250u);
        EXPECT_EQ(fake->attempts[3].size, 128u);
        auto buffer = result.unwrap_unchecked();
        EXPECT_EQ(buffer.allocation().info().occupied_size, 128u);
    }
    {
        FakeMemory context;
        fake                = &context;
        fake->fail_allocate = true;
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1024, 1024, 2, true })
                .unwrap_unchecked();
        auto result = allocator.create_buffer(BufferInfo(73));
        ASSERT_TRUE(result.is_err());
        ASSERT_EQ(fake->attempt_count, 8u);
        EXPECT_EQ(result.unwrap_err_unchecked().device_allocation_attempts, 8u);
        for (unsigned i = 0; i < 8; ++i) {
            EXPECT_EQ(fake->attempts[i].type, i < 4 ? 1u : 0u);
            EXPECT_EQ(fake->attempts[i].dedicated, i % 4 == 3);
            EXPECT_EQ(fake->attempts[i].size, i % 4 == 3 ? 73u : 1024u >> (i % 4));
        }
        EXPECT_EQ(fake->allocations, 0u);
        EXPECT_EQ(fake->creates, fake->destroys);
    }
}

TEST(Memory, DedicatedPreferenceReusesSharedUnderBudgetPressure) {
    FakeMemory context;
    fake = &context;
    auto allocator =
        MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 256, 1024, 3, true })
            .unwrap_unchecked();
    auto first = allocator.create_buffer(BufferInfo(128));
    ASSERT_TRUE(first.is_ok());
    fake->driver_budget    = true;
    fake->budget           = 1;
    fake->prefer_dedicated = true;
    auto second            = allocator.create_buffer(BufferInfo(128), { .within_budget = true });
    ASSERT_TRUE(second.is_ok());
    auto buffer = second.unwrap_unchecked();
    EXPECT_FALSE(buffer.allocation().info().dedicated);
    EXPECT_EQ(fake->attempt_count, 1u);
    EXPECT_EQ(allocator.budget().heaps[0].allocation_count, 2u);
}

TEST(Memory, DefaultPolicyGrowthAndRecovery) {
    constexpr VkDeviceSize mib = 1024 * 1024;
    for (unsigned mode = 0; mode < 4; ++mode) {
        FakeMemory context;
        fake            = &context;
        fake->heap_size = fake->max_allocation_size = fake->max_success_size = 64 * mib;
        fake->max_success_size = mode == 1 ? mib : 64 * mib;
        fake->reject_shared    = mode == 2;
        fake->reject_dedicated = mode == 3;
        fake->prefer_dedicated = mode == 3;
        {
            auto allocator =
                MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, {}).unwrap_unchecked();
            vvk::AllocatedBuffer buffers[13];
            const unsigned       count = mode == 0 ? 13 : 1;
            for (unsigned i = 0; i < count; ++i) {
                auto result = allocator.create_buffer(BufferInfo(mib));
                ASSERT_TRUE(result.is_ok());
                buffers[i] = result.unwrap_unchecked();
                EXPECT_EQ(buffers[i].allocation().info().memory_type, 1u);
                EXPECT_EQ(buffers[i].allocation().info().dedicated, mode == 2);
            }
            if (mode == 0) {
                ASSERT_EQ(fake->attempt_count, 3u);
                EXPECT_EQ(fake->attempts[0].size, 4 * mib);
                EXPECT_EQ(fake->attempts[1].size, 8 * mib);
                EXPECT_EQ(fake->attempts[2].size, 16 * mib);
            } else if (mode == 1) {
                ASSERT_EQ(fake->attempt_count, 3u);
                EXPECT_EQ(fake->attempts[0].size, 4 * mib);
                EXPECT_EQ(fake->attempts[1].size, 2 * mib);
                EXPECT_EQ(fake->attempts[2].size, mib);
            } else if (mode == 2) {
                ASSERT_EQ(fake->attempt_count, 4u);
                EXPECT_EQ(fake->attempts[2].size, mib);
                EXPECT_TRUE(fake->attempts[3].dedicated);
                EXPECT_EQ(fake->attempts[3].size, mib);
            } else {
                ASSERT_EQ(fake->attempt_count, 2u);
                EXPECT_TRUE(fake->attempts[0].dedicated);
                EXPECT_FALSE(fake->attempts[1].dedicated);
            }
            EXPECT_EQ(fake->queries, count);
        }
        EXPECT_EQ(fake->allocations, fake->frees);
        EXPECT_EQ(fake->creates, fake->destroys);
    }
}

namespace
{
void DiscreteTopology(FakeMemory& context) {
    auto& memory           = context.topology;
    memory.memoryHeapCount = 2;
    memory.memoryHeaps[0]  = { 1024 * 1024, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
    memory.memoryHeaps[1]  = { 1024 * 1024, 0 };
    memory.memoryTypeCount = 4;
    memory.memoryTypes[0]  = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    memory.memoryTypes[1]  = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1
    };
    memory.memoryTypes[2] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1
    };
    memory.memoryTypes[3]     = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  0 };
    context.requirement_types = 15;
}
} // namespace
TEST(Memory, HostAccessAndPlacementSelection) {
    FakeMemory context;
    fake = &context;
    DiscreteTopology(context);
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto check     = [&](vvk::MemoryRequest         request,
                         VkBufferUsageFlags         usage,
                         unsigned                   type,
                         vvk::MemoryPlacementReason reason) {
        auto info   = BufferInfo();
        info.usage  = usage;
        auto result = allocator.create_buffer(info, request);
        ASSERT_TRUE(result.is_ok());
        auto buffer   = result.unwrap_unchecked();
        auto selected = buffer.allocation().info();
        EXPECT_EQ(selected.memory_type, type);
        EXPECT_EQ(selected.heap, context.topology.memoryTypes[type].heapIndex);
        EXPECT_EQ(selected.selection.placement_reason, reason);
        if (request.host_access != vvk::MemoryHostAccess::None || request.persistent_mapping)
            EXPECT_TRUE(selected.selection.required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    };
    check({}, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, vvk::MemoryPlacementReason::AutomaticDevice);
    check(vvk::MemoryRequest::Upload(),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          1,
          vvk::MemoryPlacementReason::AutomaticHost);
    check(vvk::MemoryRequest::Upload(),
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          3,
          vvk::MemoryPlacementReason::AutomaticDevice);
    check(vvk::MemoryRequest::Readback(),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          2,
          vvk::MemoryPlacementReason::AutomaticHost);
    auto upload       = vvk::MemoryRequest::Upload();
    upload.preference = vvk::MemoryPreference::Host;
    check(upload, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, vvk::MemoryPlacementReason::RequestedHost);
    upload.preference = vvk::MemoryPreference::Device;
    check(upload, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 3, vvk::MemoryPlacementReason::RequestedDevice);
    check({ .persistent_mapping = true },
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          3,
          vvk::MemoryPlacementReason::AutomaticDevice);
    for (unsigned direct = 0; direct < 2; ++direct) {
        auto image_info = ImageInfo();
        if (direct) image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        auto result = allocator.create_image(image_info, vvk::MemoryRequest::Upload());
        ASSERT_TRUE(result.is_ok());
        auto image = result.unwrap_unchecked();
        EXPECT_EQ(image.allocation().info().memory_type, direct ? 3u : 1u);
    }
}

TEST(Memory, ExplicitFlagsAndHardConstraints) {
    FakeMemory context;
    fake = &context;
    DiscreteTopology(context);
    auto allocator   = MakeAllocator().unwrap_unchecked();
    auto upload      = vvk::MemoryRequest::Upload();
    upload.preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    auto result      = allocator.create_buffer(BufferInfo(), upload);
    ASSERT_TRUE(result.is_ok());
    auto buffer   = result.unwrap_unchecked();
    auto selected = buffer.allocation().info();
    EXPECT_EQ(selected.memory_type, 2u);
    EXPECT_EQ(selected.selection.explicit_preferred, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    EXPECT_TRUE(selected.selection.avoided & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    auto readback     = vvk::MemoryRequest::Readback();
    readback.required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    auto local        = allocator.create_buffer(BufferInfo(), readback);
    ASSERT_TRUE(local.is_ok());
    auto local_buffer = local.unwrap_unchecked();
    EXPECT_EQ(local_buffer.allocation().info().memory_type, 3u);
    readback.required |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    auto impossible = allocator.create_buffer(BufferInfo(), readback);
    ASSERT_TRUE(impossible.is_err());
    EXPECT_EQ(impossible.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::NoMemoryType);
    context.requirement_types = 1;
    auto restricted = allocator.create_buffer(BufferInfo(), vvk::MemoryRequest::Readback());
    ASSERT_TRUE(restricted.is_err());
    EXPECT_EQ(restricted.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::NoMemoryType);
    EXPECT_EQ(restricted.unwrap_err_unchecked().device_allocation_attempts, 0u);
    context.requirement_types = 15;
    auto flags = allocator.create_buffer(BufferInfo(),
                                         { .required   = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                           .preferred  = VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                           .preference = vvk::MemoryPreference::FlagsOnly });
    ASSERT_TRUE(flags.is_ok());
    auto flags_buffer = flags.unwrap_unchecked();
    auto detail       = flags_buffer.allocation().info();
    EXPECT_EQ(detail.memory_type, 2u);
    EXPECT_EQ(detail.selection.preferred, 0u);
    EXPECT_EQ(detail.selection.avoided, 0u);
    EXPECT_EQ(detail.selection.placement_reason, vvk::MemoryPlacementReason::ExplicitFlags);
}

TEST(Memory, FlagsOnlyWithoutInferredPreferences) {
    FakeMemory context;
    fake           = &context;
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto result =
        allocator.create_buffer(BufferInfo(), { .preference = vvk::MemoryPreference::FlagsOnly });
    ASSERT_TRUE(result.is_ok());
    auto buffer = result.unwrap_unchecked();
    EXPECT_EQ(buffer.allocation().info().memory_type, 0u);
    auto automatic = allocator.create_buffer(BufferInfo());
    ASSERT_TRUE(automatic.is_ok());
    auto automatic_buffer = automatic.unwrap_unchecked();
    EXPECT_EQ(automatic_buffer.allocation().info().memory_type, 1u);
    auto request       = vvk::MemoryRequest::Upload();
    request.preference = vvk::MemoryPreference::FlagsOnly;
    request.preferred  = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    auto mapped        = allocator.create_buffer(BufferInfo(), request);
    ASSERT_TRUE(mapped.is_ok());
    auto mapped_buffer = mapped.unwrap_unchecked();
    EXPECT_EQ(mapped_buffer.allocation().info().memory_type, 0u);
}

TEST(Memory, UnifiedMemoryAndUncachedReadback) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        FakeMemory context;
        fake                             = &context;
        context.topology.memoryHeapCount = 1;
        context.topology.memoryHeaps[0]  = { 1024 * 1024, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
        context.topology.memoryTypeCount = 1;
        context.topology.memoryTypes[0]  = {
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                (mode == 0 ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : 0u),
            0
        };
        context.requirement_types = 1;
        auto allocator            = MakeAllocator().unwrap_unchecked();
        auto request = mode == 0 ? vvk::MemoryRequest::Upload() : vvk::MemoryRequest::Readback();
        auto result  = allocator.create_buffer(BufferInfo(), request);
        ASSERT_TRUE(result.is_ok());
        auto buffer = result.unwrap_unchecked();
        EXPECT_EQ(buffer.allocation().info().memory_type, 0u);
        EXPECT_EQ(buffer.allocation().info().heap, 0u);
    }
}

TEST(Memory, SelectionFallbackAndSharedHeapAccounting) {
    FakeMemory context;
    fake = &context;
    DiscreteTopology(context);
    auto allocator = MakeAllocator().unwrap_unchecked();
    auto readback  = allocator.create_buffer(BufferInfo(), vvk::MemoryRequest::Readback());
    ASSERT_TRUE(readback.is_ok());
    auto first   = readback.unwrap_unchecked();
    auto memory  = first.allocation();
    auto mapping = memory.map();
    ASSERT_TRUE(mapping.is_ok());
    EXPECT_TRUE(memory.invalidate().is_ok());
    EXPECT_EQ(context.invalidates, 1u);
    auto upload = allocator.create_buffer(BufferInfo(), vvk::MemoryRequest::Upload());
    ASSERT_TRUE(upload.is_ok());
    auto second   = upload.unwrap_unchecked();
    auto snapshot = allocator.budget();
    EXPECT_EQ(snapshot.heaps[1].block_count, 2u);
    EXPECT_EQ(snapshot.heaps[1].allocation_count, 2u);
    context.reject_types = 1 << 2;
    auto fallback        = allocator.create_buffer(
        BufferInfo(512), { .dedicated = true, .host_access = vvk::MemoryHostAccess::Random });
    ASSERT_TRUE(fallback.is_ok());
    auto third    = fallback.unwrap_unchecked();
    auto selected = third.allocation().info();
    EXPECT_EQ(selected.memory_type, 1u);
    EXPECT_TRUE(selected.selection.type_fallback);
    EXPECT_EQ(selected.selection.device_allocation_attempts, 2u);
    EXPECT_EQ(selected.selection.dedicated_reason, vvk::MemoryDedicatedReason::Requested);
    auto lease = third.allocation();
    third.reset();
    EXPECT_EQ(lease.info().selection.dedicated_reason, vvk::MemoryDedicatedReason::Requested);
}

TEST(Memory, DedicatedSelectionReasons) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        FakeMemory context;
        fake                     = &context;
        context.dedicated        = mode == 0;
        context.prefer_dedicated = mode == 2;
        context.reject_shared    = mode == 4;
        auto allocator =
            MakeAllocator(alloc::allocator_ref(alloc::GLOBAL), false, { 1024, 1024, 0, true })
                .unwrap_unchecked();
        auto result =
            allocator.create_buffer(BufferInfo(mode == 3 ? 600 : 73), { .dedicated = mode == 1 });
        ASSERT_TRUE(result.is_ok());
        auto                             buffer = result.unwrap_unchecked();
        const vvk::MemoryDedicatedReason reasons[] { vvk::MemoryDedicatedReason::Required,
                                                     vvk::MemoryDedicatedReason::Requested,
                                                     vvk::MemoryDedicatedReason::DriverPreferred,
                                                     vvk::MemoryDedicatedReason::LargeResource,
                                                     vvk::MemoryDedicatedReason::SharedExhausted };
        EXPECT_EQ(buffer.allocation().info().selection.dedicated_reason, reasons[mode]);
    }
}

TEST(Memory, InvalidAccessIntentRollsBack) {
    FakeMemory context;
    fake                = &context;
    auto allocator      = MakeAllocator().unwrap_unchecked();
    auto invalid        = vvk::MemoryRequest {};
    invalid.host_access = static_cast<vvk::MemoryHostAccess>(99);
    auto first          = allocator.create_buffer(BufferInfo(), invalid);
    ASSERT_TRUE(first.is_err());
    EXPECT_EQ(first.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
    invalid            = {};
    invalid.preference = static_cast<vvk::MemoryPreference>(99);
    auto second        = allocator.create_image(ImageInfo(), invalid);
    ASSERT_TRUE(second.is_err());
    EXPECT_EQ(second.unwrap_err_unchecked().kind, vvk::MemoryErrorKind::InvalidRequest);
    EXPECT_EQ(fake->attempt_count, 0u);
    EXPECT_EQ(fake->creates, fake->destroys);
}
