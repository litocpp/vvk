module;
#include <vulkan/vulkan.h>

export module vvk:memory;
import rstd;
export import :ffi.vulkan;
import :dispatch;

using namespace rstd::prelude;

export namespace vvk
{

/// All handles from one allocator require external synchronization. The device and
/// metadata allocator must outlive the allocator, resources, allocations and mappings.
struct MemoryDispatch {
    PFN_vkGetPhysicalDeviceProperties        properties {};
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory_properties {};
    PFN_vkAllocateMemory                     allocate {};
    PFN_vkFreeMemory                         free {};
    PFN_vkCreateBuffer                       create_buffer {};
    PFN_vkDestroyBuffer                      destroy_buffer {};
    PFN_vkCreateImage                        create_image {};
    PFN_vkDestroyImage                       destroy_image {};
    PFN_vkGetBufferMemoryRequirements2       buffer_requirements {};
    PFN_vkGetImageMemoryRequirements2        image_requirements {};
    PFN_vkBindBufferMemory                   bind_buffer {};
    PFN_vkBindImageMemory                    bind_image {};
    PFN_vkMapMemory                          map {};
    PFN_vkUnmapMemory                        unmap {};
    PFN_vkFlushMappedMemoryRanges            flush {};
    PFN_vkInvalidateMappedMemoryRanges       invalidate {};
    static MemoryDispatch                    FromDispatch(const InstanceDispatch& instance,
                                                          const DeviceDispatch&   dispatch) noexcept {
        return { instance.vkGetPhysicalDeviceProperties,
                 instance.vkGetPhysicalDeviceMemoryProperties2,
                 dispatch.vkAllocateMemory,
                 dispatch.vkFreeMemory,
                 dispatch.vkCreateBuffer,
                 dispatch.vkDestroyBuffer,
                 dispatch.vkCreateImage,
                 dispatch.vkDestroyImage,
                 dispatch.vkGetBufferMemoryRequirements2,
                 dispatch.vkGetImageMemoryRequirements2,
                 dispatch.vkBindBufferMemory,
                 dispatch.vkBindImageMemory,
                 dispatch.vkMapMemory,
                 dispatch.vkUnmapMemory,
                 dispatch.vkFlushMappedMemoryRanges,
                 dispatch.vkInvalidateMappedMemoryRanges };
    }
    bool valid() const noexcept {
        return properties && memory_properties && allocate && free && create_buffer &&
               destroy_buffer && create_image && destroy_image && buffer_requirements &&
               image_requirements && bind_buffer && bind_image && map && unmap && flush &&
               invalidate;
    }
};

enum class MemoryErrorKind
{
    InvalidRequest,
    Unsupported,
    NoMemoryType,
    HostMemory,
    DeviceMemory,
    Vulkan
};
struct MemoryError {
    MemoryErrorKind kind;
    VkResult        api_result { VK_SUCCESS };
};
using MemoryMetadata = rstd::ref<rstd::dyn<rstd::alloc::Allocator>>;

struct MemoryAllocatorCreateInfo {
    VkPhysicalDevice physical_device {};
    VkDevice         device {};
    VkDeviceSize     block_size { 16 * 1024 * 1024 };
    // Set only when VK_EXT_memory_budget was enabled on this device.
    bool           memory_budget_enabled {};
    MemoryDispatch dispatch {};
    MemoryAllocatorCreateInfo() = default;
    MemoryAllocatorCreateInfo(VkPhysicalDevice physical, VkDevice device, VkDeviceSize block,
                              bool budget, MemoryDispatch dispatch)
        : physical_device(physical),
          device(device),
          block_size(block),
          memory_budget_enabled(budget),
          dispatch(dispatch) {}
};
struct MemoryRequest {
    VkMemoryPropertyFlags required {};
    VkMemoryPropertyFlags preferred { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT };
    bool                  dedicated {};
    bool                  persistent_mapping {};
    bool                  within_budget {};
};
struct MemoryInfo {
    VkDeviceMemory        memory {};
    VkDeviceSize          offset {}, size {}, occupied_size {};
    rstd::uint32_t        memory_type {};
    VkMemoryPropertyFlags properties {};
    bool                  dedicated {};
};
enum class MemoryBudgetSource
{
    AllocatorEstimate,
    Driver
};
struct MemoryHeapBudget {
    VkDeviceSize       heap_size {}, usage {}, budget {}, block_bytes {}, allocation_bytes {};
    rstd::uint32_t     block_count {}, allocation_count {};
    MemoryBudgetSource source { MemoryBudgetSource::AllocatorEstimate };
};
struct MemoryBudgetSnapshot {
    MemoryHeapBudget heaps[VK_MAX_MEMORY_HEAPS] {};
    rstd::uint32_t   heap_count {};
};

class MemoryAllocator;
class MemoryAllocation;
class MemoryMapping;
class AllocatedBuffer;
class AllocatedImage;
} // namespace vvk

namespace vvk
{
struct MemoryState;
struct MemoryBlock;
struct MemoryRegion;
struct MemoryResource;

MemoryError HostMemoryError() {
    return { MemoryErrorKind::HostMemory, VK_ERROR_OUT_OF_HOST_MEMORY };
}
MemoryError ApiMemoryError(VkResult result) {
    return { result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? MemoryErrorKind::DeviceMemory
             : result == VK_ERROR_OUT_OF_HOST_MEMORY ? MemoryErrorKind::HostMemory
                                                     : MemoryErrorKind::Vulkan,
             result };
}
template<typename T, typename... Args>
T* NewMemoryObject(MemoryMetadata metadata, Args&&... args) {
    auto allocation = metadata->allocate(rstd::alloc::Layout::make<T>());
    if (allocation.is_err()) return nullptr;
    auto pointer = allocation.unwrap_unchecked().template as_mut_ptr<T>();
    rstd::ptr_::construct(pointer, rstd::forward<Args>(args)...);
    return pointer.as_raw_ptr();
}
template<typename T>
void DeleteMemoryObject(MemoryMetadata metadata, T* object) {
    object->~T();
    metadata->deallocate(object, rstd::alloc::Layout::make<T>());
}

enum class MemoryClass
{
    Linear,
    Optimal
};
struct MemoryBlock {
    MemoryState*                          owner;
    VkDeviceMemory                        memory {};
    VkDeviceSize                          size;
    rstd::uint32_t                        type;
    MemoryClass                           resource_class;
    bool                                  dedicated;
    alloc::RangeAllocator<MemoryMetadata> ranges;
    void*                                 mapped {};
    rstd::size_t                          maps {};
    MemoryBlock(MemoryState* owner, VkDeviceSize size, rstd::uint32_t type, MemoryClass cls,
                bool dedicated, MemoryMetadata metadata)
        : owner(owner),
          size(size),
          type(type),
          resource_class(cls),
          dedicated(dedicated),
          ranges(size, metadata) {}
};
struct MemoryState {
    MemoryAllocatorCreateInfo                     info;
    MemoryMetadata                                metadata;
    VkPhysicalDeviceProperties                    properties {};
    VkPhysicalDeviceMemoryProperties              memory {};
    alloc::vec::Vec<MemoryBlock*, MemoryMetadata> blocks;
    rstd::size_t                                  refs { 1 };
    explicit MemoryState(MemoryAllocatorCreateInfo info, MemoryMetadata metadata)
        : info(info),
          metadata(metadata),
          blocks(alloc::vec::Vec<MemoryBlock*, MemoryMetadata>::new_in(metadata)) {}
};
struct MemoryRegion {
    MemoryState*           owner;
    MemoryBlock*           block;
    alloc::RangeAllocation range;
    VkDeviceSize           requested;
    rstd::size_t           refs { 1 };
    bool                   persistent {};
    MemoryRegion(MemoryState* owner, MemoryBlock* block, alloc::RangeAllocation range,
                 VkDeviceSize requested)
        : owner(owner), block(block), range(range), requested(requested) {
        ++owner->refs;
    }
};
void DestroyMemoryBlock(MemoryState* owner, MemoryBlock* block) {
    if (block->mapped) owner->info.dispatch.unmap(owner->info.device, block->memory);
    owner->info.dispatch.free(owner->info.device, block->memory, nullptr);
    DeleteMemoryObject(owner->metadata, block);
}
void DropMemoryState(MemoryState* owner) {
    if (--owner->refs != 0) return;
    for (auto* block : owner->blocks) DestroyMemoryBlock(owner, block);
    const auto metadata = owner->metadata;
    DeleteMemoryObject(metadata, owner);
}
void UnmapMemoryBlock(MemoryBlock* block) {
    if (--block->maps == 0) {
        block->owner->info.dispatch.unmap(block->owner->info.device, block->memory);
        block->mapped = nullptr;
    }
}
VkResult MapMemoryBlock(MemoryBlock* block) {
    if (block->maps == 0) {
        void* mapped = nullptr;
        auto  result = block->owner->info.dispatch.map(
            block->owner->info.device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        block->mapped = mapped;
    }
    ++block->maps;
    return VK_SUCCESS;
}
void DropMemoryRegion(MemoryRegion* region) {
    if (--region->refs != 0) return;
    auto* owner = region->owner;
    auto* block = region->block;
    if (region->persistent) UnmapMemoryBlock(block);
    block->ranges.deallocate(region->range.id);
    bool release_block = block->dedicated;
    if (! release_block && block->ranges.counters().allocation_count == 0) {
        for (auto* other : owner->blocks) {
            if (other != block && ! other->dedicated && other->type == block->type &&
                other->resource_class == block->resource_class &&
                other->ranges.counters().allocation_count == 0) {
                release_block = true;
                break;
            }
        }
    }
    if (release_block) {
        for (usize i {}; i < owner->blocks.len(); ++i)
            if (owner->blocks[i] == block) {
                owner->blocks.remove(i);
                break;
            }
        DestroyMemoryBlock(owner, block);
    }
    DeleteMemoryObject(owner->metadata, region);
    DropMemoryState(owner);
}

/// A memory lease preserves storage, not its bound buffer or image. Retain the
/// resource lease until every submission using that resource has completed.
export class MemoryAllocation {
    MemoryRegion* region_ {};
    explicit MemoryAllocation(MemoryRegion* region): region_(region) {}
    friend class MemoryAllocator;
    friend class MemoryMapping;

public:
    MemoryAllocation()                                   = default;
    MemoryAllocation(const MemoryAllocation&)            = delete;
    MemoryAllocation& operator=(const MemoryAllocation&) = delete;
    MemoryAllocation(MemoryAllocation&& other) noexcept
        : region_(rstd::exchange(other.region_, nullptr)) {}
    MemoryAllocation& operator=(MemoryAllocation&& other) noexcept {
        if (this != &other) {
            reset();
            region_ = rstd::exchange(other.region_, nullptr);
        }
        return *this;
    }
    ~MemoryAllocation() { reset(); }
    void reset() {
        if (region_) {
            DropMemoryRegion(region_);
            region_ = nullptr;
        }
    }
    bool             valid() const noexcept { return region_ != nullptr; }
    MemoryAllocation clone() const {
        if (region_) ++region_->refs;
        return MemoryAllocation(region_);
    }
    auto info() const noexcept -> MemoryInfo {
        if (! region_) return {};
        const auto* block = region_->block;
        return { block->memory,      region_->range.offset,
                 region_->requested, region_->range.size,
                 block->type,        region_->owner->memory.memoryTypes[block->type].propertyFlags,
                 block->dedicated };
    }
    auto map(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const
        -> Result<MemoryMapping, MemoryError>;
    auto flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const
        -> Result<empty, MemoryError>;
    auto invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const
        -> Result<empty, MemoryError>;

private:
    auto cache_operation(VkDeviceSize offset, VkDeviceSize size, bool invalidate) const
        -> Result<empty, MemoryError>;
};

export class MemoryMapping {
    MemoryAllocation allocation_;
    void*            data_ {};
    VkDeviceSize     size_ {};
    MemoryMapping(MemoryAllocation allocation, void* data, VkDeviceSize size)
        : allocation_(rstd::move(allocation)), data_(data), size_(size) {}
    friend class MemoryAllocation;

public:
    MemoryMapping(const MemoryMapping&)            = delete;
    MemoryMapping& operator=(const MemoryMapping&) = delete;
    MemoryMapping(MemoryMapping&& other) noexcept
        : allocation_(rstd::move(other.allocation_)),
          data_(rstd::exchange(other.data_, nullptr)),
          size_(other.size_) {}
    MemoryMapping& operator=(MemoryMapping&& other) noexcept {
        if (this != &other) {
            if (allocation_.valid()) UnmapMemoryBlock(allocation_.region_->block);
            allocation_ = rstd::move(other.allocation_);
            data_       = rstd::exchange(other.data_, nullptr);
            size_       = other.size_;
        }
        return *this;
    }
    ~MemoryMapping() {
        if (allocation_.valid()) UnmapMemoryBlock(allocation_.region_->block);
    }
    void*        data() const noexcept { return data_; }
    VkDeviceSize size() const noexcept { return size_; }
};

auto MemoryAllocation::map(VkDeviceSize offset, VkDeviceSize size) const
    -> Result<MemoryMapping, MemoryError> {
    if (! region_ || offset > region_->requested)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (size == VK_WHOLE_SIZE) size = region_->requested - offset;
    if (size == 0 || size > region_->requested - offset ||
        (info().properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (offset > rstd::size_t(-1) || region_->range.offset > rstd::size_t(-1) - offset ||
        size > rstd::size_t(-1) - region_->range.offset - offset)
        return Err(MemoryError { MemoryErrorKind::Unsupported });
    auto result = MapMemoryBlock(region_->block);
    if (result != VK_SUCCESS) return Err(ApiMemoryError(result));
    auto pointer =
        static_cast<rstd::uint8_t*>(region_->block->mapped) + region_->range.offset + offset;
    return Ok(MemoryMapping(clone(), pointer, size));
}
auto MemoryAllocation::cache_operation(VkDeviceSize offset, VkDeviceSize size,
                                       bool invalidating) const -> Result<empty, MemoryError> {
    if (! region_ || offset > region_->requested)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (size == VK_WHOLE_SIZE) size = region_->requested - offset;
    if (size > region_->requested - offset)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (size == 0) return Ok(empty {});
    if (region_->block->maps == 0) return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (info().properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return Ok(empty {});
    auto* owner = region_->owner;
    auto  atom  = owner->properties.limits.nonCoherentAtomSize;
    auto  start = region_->range.offset + offset;
    auto  end   = start + size;
    start -= start % atom;
    const auto remainder = end % atom;
    if (remainder) {
        const auto padding = atom - remainder;
        end = padding > region_->block->size - end ? region_->block->size : end + padding;
    }
    VkMappedMemoryRange range {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, region_->block->memory, start, end - start
    };
    auto result = (invalidating ? owner->info.dispatch.invalidate
                                : owner->info.dispatch.flush)(owner->info.device, 1, &range);
    if (result != VK_SUCCESS) return Err(ApiMemoryError(result));
    return Ok(empty {});
}
auto MemoryAllocation::flush(VkDeviceSize offset, VkDeviceSize size) const
    -> Result<empty, MemoryError> {
    return cache_operation(offset, size, false);
}
auto MemoryAllocation::invalidate(VkDeviceSize offset, VkDeviceSize size) const
    -> Result<empty, MemoryError> {
    return cache_operation(offset, size, true);
}

struct MemoryResource {
    MemoryAllocation allocation;
    VkBuffer         buffer {};
    VkImage          image {};
    MemoryState*     owner;
    rstd::size_t     refs { 1 };
    MemoryResource(MemoryAllocation allocation, VkBuffer buffer, VkImage image, MemoryState* owner)
        : allocation(rstd::move(allocation)), buffer(buffer), image(image), owner(owner) {}
};
void DropMemoryResource(MemoryResource* resource) {
    if (--resource->refs != 0) return;
    auto* owner    = resource->owner;
    auto  metadata = owner->metadata;
    if (resource->buffer)
        owner->info.dispatch.destroy_buffer(owner->info.device, resource->buffer, nullptr);
    if (resource->image)
        owner->info.dispatch.destroy_image(owner->info.device, resource->image, nullptr);
    DeleteMemoryObject(metadata, resource);
}

export class AllocatedBuffer {
    MemoryResource* resource_ {};
    explicit AllocatedBuffer(MemoryResource* resource): resource_(resource) {}
    friend class MemoryAllocator;

public:
    AllocatedBuffer()                                  = default;
    AllocatedBuffer(const AllocatedBuffer&)            = delete;
    AllocatedBuffer& operator=(const AllocatedBuffer&) = delete;
    AllocatedBuffer(AllocatedBuffer&& other) noexcept
        : resource_(rstd::exchange(other.resource_, nullptr)) {}
    AllocatedBuffer& operator=(AllocatedBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            resource_ = rstd::exchange(other.resource_, nullptr);
        }
        return *this;
    }
    ~AllocatedBuffer() { reset(); }
    void reset() {
        if (resource_) {
            DropMemoryResource(resource_);
            resource_ = nullptr;
        }
    }
    bool     valid() const noexcept { return resource_ != nullptr; }
    VkBuffer handle() const noexcept { return resource_ ? resource_->buffer : VK_NULL_HANDLE; }
    AllocatedBuffer clone() const {
        if (resource_) ++resource_->refs;
        return AllocatedBuffer(resource_);
    }
    MemoryAllocation allocation() const {
        return resource_ ? resource_->allocation.clone() : MemoryAllocation();
    }
};
export class AllocatedImage {
    MemoryResource* resource_ {};
    explicit AllocatedImage(MemoryResource* resource): resource_(resource) {}
    friend class MemoryAllocator;

public:
    AllocatedImage()                                 = default;
    AllocatedImage(const AllocatedImage&)            = delete;
    AllocatedImage& operator=(const AllocatedImage&) = delete;
    AllocatedImage(AllocatedImage&& other) noexcept
        : resource_(rstd::exchange(other.resource_, nullptr)) {}
    AllocatedImage& operator=(AllocatedImage&& other) noexcept {
        if (this != &other) {
            reset();
            resource_ = rstd::exchange(other.resource_, nullptr);
        }
        return *this;
    }
    ~AllocatedImage() { reset(); }
    void reset() {
        if (resource_) {
            DropMemoryResource(resource_);
            resource_ = nullptr;
        }
    }
    bool           valid() const noexcept { return resource_ != nullptr; }
    VkImage        handle() const noexcept { return resource_ ? resource_->image : VK_NULL_HANDLE; }
    AllocatedImage clone() const {
        if (resource_) ++resource_->refs;
        return AllocatedImage(resource_);
    }
    MemoryAllocation allocation() const {
        return resource_ ? resource_->allocation.clone() : MemoryAllocation();
    }
};

export class MemoryAllocator {
    MemoryState* state_ {};
    explicit MemoryAllocator(MemoryState* state): state_(state) {}
    auto allocate(const VkMemoryRequirements&          requirements,
                  const VkMemoryDedicatedRequirements& dedicated, MemoryClass cls, VkBuffer buffer,
                  VkImage image, MemoryRequest request) const
        -> Result<MemoryAllocation, MemoryError>;

public:
    MemoryAllocator()                                  = default;
    MemoryAllocator(const MemoryAllocator&)            = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;
    MemoryAllocator(MemoryAllocator&& other) noexcept
        : state_(rstd::exchange(other.state_, nullptr)) {}
    MemoryAllocator& operator=(MemoryAllocator&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = rstd::exchange(other.state_, nullptr);
        }
        return *this;
    }
    ~MemoryAllocator() { reset(); }
    void reset() {
        if (state_) {
            DropMemoryState(state_);
            state_ = nullptr;
        }
    }
    static auto Create(VkPhysicalDevice physical, const InstanceDispatch& instance,
                       const DeviceDispatch& device, VkDeviceSize block_size = 16 * 1024 * 1024,
                       MemoryMetadata metadata = alloc::allocator_ref(alloc::GLOBAL))
        -> Result<MemoryAllocator, MemoryError> {
        if (! instance.instance || device.instance != instance.instance)
            return Err(MemoryError { MemoryErrorKind::InvalidRequest });
        return Create({ physical,
                        device.device,
                        block_size,
                        device.capabilities.memory_budget,
                        MemoryDispatch::FromDispatch(instance, device) },
                      metadata);
    }
    static auto Create(MemoryAllocatorCreateInfo info,
                       MemoryMetadata            metadata = alloc::allocator_ref(alloc::GLOBAL))
        -> Result<MemoryAllocator, MemoryError> {
        if (! info.device || ! info.physical_device || ! info.block_size || ! info.dispatch.valid())
            return Err(MemoryError { MemoryErrorKind::InvalidRequest });
        auto* state = NewMemoryObject<MemoryState>(metadata, info, metadata);
        if (! state) return Err(HostMemoryError());
        info.dispatch.properties(info.physical_device, &state->properties);
        VkPhysicalDeviceMemoryProperties2 properties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2
        };
        info.dispatch.memory_properties(info.physical_device, &properties);
        state->memory = properties.memoryProperties;
        if (state->properties.apiVersion < VK_API_VERSION_1_1 ||
            state->memory.memoryTypeCount == 0 ||
            state->properties.limits.nonCoherentAtomSize == 0) {
            DropMemoryState(state);
            return Err(MemoryError { MemoryErrorKind::Unsupported });
        }
        return Ok(MemoryAllocator(state));
    }
    auto create_buffer(const VkBufferCreateInfo& info, MemoryRequest request = {}) const
        -> Result<AllocatedBuffer, MemoryError>;
    auto create_image(const VkImageCreateInfo& info, MemoryRequest request = {}) const
        -> Result<AllocatedImage, MemoryError>;
    auto budget() const -> MemoryBudgetSnapshot;
    void trim() const {
        if (! state_) return;
        for (usize i {}; i < state_->blocks.len();) {
            auto* block = state_->blocks[i];
            if (block->ranges.counters().allocation_count == 0) {
                state_->blocks.remove(i);
                DestroyMemoryBlock(state_, block);
            } else
                ++i;
        }
    }
};

auto MemoryAllocator::budget() const -> MemoryBudgetSnapshot {
    MemoryBudgetSnapshot result;
    if (! state_) return result;
    result.heap_count = state_->memory.memoryHeapCount;
    for (rstd::uint32_t i = 0; i < result.heap_count; ++i) {
        result.heaps[i].heap_size = state_->memory.memoryHeaps[i].size;
        result.heaps[i].budget    = result.heaps[i].heap_size;
    }
    for (auto* block : state_->blocks) {
        auto& heap = result.heaps[state_->memory.memoryTypes[block->type].heapIndex];
        heap.block_bytes += block->size;
        ++heap.block_count;
        const auto stats = block->ranges.counters();
        heap.allocation_bytes += stats.occupied_bytes;
        heap.allocation_count += rstd::uint32_t(stats.allocation_count);
    }
    for (rstd::uint32_t i = 0; i < result.heap_count; ++i)
        result.heaps[i].usage = result.heaps[i].block_bytes;
    if (state_->info.memory_budget_enabled) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT driver {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
        };
        VkPhysicalDeviceMemoryProperties2 properties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &driver
        };
        state_->info.dispatch.memory_properties(state_->info.physical_device, &properties);
        for (rstd::uint32_t i = 0; i < result.heap_count; ++i)
            if (driver.heapBudget[i] != 0) {
                auto& heap = result.heaps[i];
                heap.usage = driver.heapUsage[i];
                heap.budget =
                    driver.heapBudget[i] < heap.heap_size ? driver.heapBudget[i] : heap.heap_size;
                heap.source = MemoryBudgetSource::Driver;
            }
    }
    return result;
}

auto MemoryAllocator::allocate(const VkMemoryRequirements&          req,
                               const VkMemoryDedicatedRequirements& dedicated, MemoryClass cls,
                               VkBuffer buffer, VkImage image, MemoryRequest request) const
    -> Result<MemoryAllocation, MemoryError> {
    if (! alloc::ValidRangeLayout(req.size, req.alignment))
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    if (request.persistent_mapping) request.required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    rstd::uint32_t candidates = req.memoryTypeBits;
    MemoryError    last { MemoryErrorKind::NoMemoryType, VK_ERROR_FEATURE_NOT_PRESENT };
    while (candidates) {
        rstd::uint32_t selected = VK_MAX_MEMORY_TYPES, best = ~rstd::uint32_t(0);
        for (rstd::uint32_t i = 0; i < state_->memory.memoryTypeCount; ++i) {
            auto flags = state_->memory.memoryTypes[i].propertyFlags;
            if (! (candidates & (1U << i)) || (flags & request.required) != request.required ||
                (flags &
                 (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)))
                continue;
            auto score = rstd::uint32_t(__builtin_popcount(request.preferred & ~flags));
            if (score < best) {
                best     = score;
                selected = i;
            }
        }
        if (selected == VK_MAX_MEMORY_TYPES) break;
        candidates &= ~(1U << selected);
        auto               flags     = state_->memory.memoryTypes[selected].propertyFlags;
        const bool         separate  = request.dedicated || dedicated.requiresDedicatedAllocation ||
                                       dedicated.prefersDedicatedAllocation ||
                                       req.size > state_->info.block_size / 2;
        const VkDeviceSize atom      = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                                               ! (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                           ? state_->properties.limits.nonCoherentAtomSize
                                           : 1;
        auto               alignment = req.alignment > atom ? req.alignment : atom;
        auto               size      = req.size;
        if (! separate && size % atom) {
            if (size > ~VkDeviceSize(0) - (atom - size % atom))
                return Err(MemoryError { MemoryErrorKind::InvalidRequest });
            size += atom - size % atom;
        }
        MemoryBlock*           block = nullptr;
        alloc::RangeAllocation range;
        if (! separate)
            for (auto* candidate : state_->blocks) {
                if (candidate->dedicated || candidate->type != selected ||
                    candidate->resource_class != cls)
                    continue;
                auto allocation = candidate->ranges.allocate(size, alignment);
                if (allocation.is_ok()) {
                    block = candidate;
                    range = allocation.unwrap_unchecked();
                    break;
                }
                if (allocation.unwrap_err_unchecked() == alloc::RangeError::MetadataAllocation)
                    return Err(HostMemoryError());
            }
        if (! block) {
            const auto bytes =
                separate ? size : (state_->info.block_size > size ? state_->info.block_size : size);
            if (state_->blocks.len().to_primitive() >=
                state_->properties.limits.maxMemoryAllocationCount) {
                last = { MemoryErrorKind::DeviceMemory, VK_ERROR_TOO_MANY_OBJECTS };
                continue;
            }
            if (request.within_budget) {
                const auto snapshot = budget();
                const auto heap = snapshot.heaps[state_->memory.memoryTypes[selected].heapIndex];
                if (heap.usage > heap.budget || bytes > heap.budget - heap.usage) {
                    last = ApiMemoryError(VK_ERROR_OUT_OF_DEVICE_MEMORY);
                    continue;
                }
            }
            if (state_->blocks.try_reserve(usize(1)).is_err()) return Err(HostMemoryError());
            block = NewMemoryObject<MemoryBlock>(
                state_->metadata, state_, bytes, selected, cls, separate, state_->metadata);
            if (! block) return Err(HostMemoryError());
            auto allocation = block->ranges.allocate(size, alignment);
            if (allocation.is_err()) {
                DeleteMemoryObject(state_->metadata, block);
                return Err(HostMemoryError());
            }
            range = allocation.unwrap_unchecked();
            VkMemoryDedicatedAllocateInfo dedicated_info {
                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image, buffer
            };
            VkMemoryAllocateInfo info { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        separate ? &dedicated_info : nullptr,
                                        bytes,
                                        selected };
            auto                 result =
                state_->info.dispatch.allocate(state_->info.device, &info, nullptr, &block->memory);
            if (result != VK_SUCCESS) {
                DeleteMemoryObject(state_->metadata, block);
                last = ApiMemoryError(result);
                if (result == VK_ERROR_DEVICE_LOST) return Err(last);
                continue;
            }
            state_->blocks.push(rstd::move(block));
            block = state_->blocks[state_->blocks.len() - usize(1)];
        }
        auto* region =
            NewMemoryObject<MemoryRegion>(state_->metadata, state_, block, range, req.size);
        if (! region) {
            block->ranges.deallocate(range.id);
            trim();
            return Err(HostMemoryError());
        }
        MemoryAllocation result(region);
        if (request.persistent_mapping) {
            auto mapped = MapMemoryBlock(block);
            if (mapped != VK_SUCCESS) return Err(ApiMemoryError(mapped));
            region->persistent = true;
        }
        return Ok(rstd::move(result));
    }
    return Err(last);
}

auto MemoryAllocator::create_buffer(const VkBufferCreateInfo& info, MemoryRequest request) const
    -> Result<AllocatedBuffer, MemoryError> {
    if (! state_ || info.sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || info.size == 0 ||
        info.usage == 0)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    constexpr VkBufferUsageFlags supported =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (info.flags || info.pNext || (info.usage & ~supported))
        return Err(MemoryError { MemoryErrorKind::Unsupported });
    VkBuffer buffer {};
    auto result = state_->info.dispatch.create_buffer(state_->info.device, &info, nullptr, &buffer);
    if (result != VK_SUCCESS) return Err(ApiMemoryError(result));
    VkMemoryDedicatedRequirements dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 requirements { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated };
    VkBufferMemoryRequirementsInfo2 query { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                                            nullptr,
                                            buffer };
    state_->info.dispatch.buffer_requirements(state_->info.device, &query, &requirements);
    auto allocation = allocate(requirements.memoryRequirements,
                               dedicated,
                               MemoryClass::Linear,
                               buffer,
                               VK_NULL_HANDLE,
                               request);
    if (allocation.is_err()) {
        state_->info.dispatch.destroy_buffer(state_->info.device, buffer, nullptr);
        return Err(allocation.unwrap_err_unchecked());
    }
    auto memory      = allocation.unwrap_unchecked();
    auto memory_info = memory.info();
    result           = state_->info.dispatch.bind_buffer(
        state_->info.device, buffer, memory_info.memory, memory_info.offset);
    if (result != VK_SUCCESS) {
        state_->info.dispatch.destroy_buffer(state_->info.device, buffer, nullptr);
        return Err(ApiMemoryError(result));
    }
    auto* resource = NewMemoryObject<MemoryResource>(
        state_->metadata, rstd::move(memory), buffer, VK_NULL_HANDLE, state_);
    if (! resource) {
        state_->info.dispatch.destroy_buffer(state_->info.device, buffer, nullptr);
        return Err(HostMemoryError());
    }
    return Ok(AllocatedBuffer(resource));
}
auto MemoryAllocator::create_image(const VkImageCreateInfo& info, MemoryRequest request) const
    -> Result<AllocatedImage, MemoryError> {
    if (! state_ || info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || ! info.extent.width ||
        ! info.extent.height || ! info.extent.depth || ! info.mipLevels || ! info.arrayLayers ||
        ! info.usage)
        return Err(MemoryError { MemoryErrorKind::InvalidRequest });
    constexpr VkImageUsageFlags supported =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (info.flags || info.pNext || (info.usage & ~supported) ||
        (info.tiling != VK_IMAGE_TILING_LINEAR && info.tiling != VK_IMAGE_TILING_OPTIMAL))
        return Err(MemoryError { MemoryErrorKind::Unsupported });
    VkImage image {};
    auto result = state_->info.dispatch.create_image(state_->info.device, &info, nullptr, &image);
    if (result != VK_SUCCESS) return Err(ApiMemoryError(result));
    VkMemoryDedicatedRequirements dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 requirements { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated };
    VkImageMemoryRequirementsInfo2 query { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                                           nullptr,
                                           image };
    state_->info.dispatch.image_requirements(state_->info.device, &query, &requirements);
    auto allocation = allocate(requirements.memoryRequirements,
                               dedicated,
                               info.tiling == VK_IMAGE_TILING_OPTIMAL ? MemoryClass::Optimal
                                                                      : MemoryClass::Linear,
                               VK_NULL_HANDLE,
                               image,
                               request);
    if (allocation.is_err()) {
        state_->info.dispatch.destroy_image(state_->info.device, image, nullptr);
        return Err(allocation.unwrap_err_unchecked());
    }
    auto memory      = allocation.unwrap_unchecked();
    auto memory_info = memory.info();
    result           = state_->info.dispatch.bind_image(
        state_->info.device, image, memory_info.memory, memory_info.offset);
    if (result != VK_SUCCESS) {
        state_->info.dispatch.destroy_image(state_->info.device, image, nullptr);
        return Err(ApiMemoryError(result));
    }
    auto* resource = NewMemoryObject<MemoryResource>(
        state_->metadata, rstd::move(memory), VK_NULL_HANDLE, image, state_);
    if (! resource) {
        state_->info.dispatch.destroy_image(state_->info.device, image, nullptr);
        return Err(HostMemoryError());
    }
    return Ok(AllocatedImage(resource));
}
} // namespace vvk
