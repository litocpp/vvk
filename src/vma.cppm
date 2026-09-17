module;
#include <vulkan/vulkan.h>
export module vvk:vma;

import rstd.cppstd;
import :handle;
import :dispatch;
export import :ffi.vma;

export namespace vvk
{

struct VmaOwner {
    VmaAllocator      allocator {};
    VmaAllocation     allocation {};
    VmaAllocationInfo allocationInfo {};

    VmaOwner()  = default;
    ~VmaOwner() = default;

    VmaOwner(std::nullptr_t) {}
    VmaOwner& operator=(std::nullptr_t) {
        allocator      = {};
        allocation     = {};
        allocationInfo = {};
        return *this;
    }
};

inline void Destroy(VmaAllocator allocator, int) { vmaDestroyAllocator(allocator); }
inline void Destroy(VmaOwner owner, VkBuffer handle, int) {
    vmaDestroyBuffer(owner.allocator, handle, owner.allocation);
}
inline void Destroy(VmaOwner owner, VkImage handle, int) {
    vmaDestroyImage(owner.allocator, handle, owner.allocation);
}

class VmaAllocatorHandle : NoCopy {
public:
    VmaAllocatorHandle() = default;
    VmaAllocatorHandle(VmaAllocator vma, int): m_allocator(vma) {}
    ~VmaAllocatorHandle() { Release(); }

    VmaAllocatorHandle(VmaAllocatorHandle&& o)
        : m_allocator(std::exchange(o.m_allocator, nullptr)) {}
    VmaAllocatorHandle& operator=(VmaAllocatorHandle&& o) noexcept {
        Release();
        m_allocator = std::exchange(o.m_allocator, nullptr);
        return *this;
    }

    const auto& operator*() const noexcept { return m_allocator; }

private:
    void Release() {
        if (m_allocator) Destroy(m_allocator, 0);
    }
    VmaAllocator m_allocator {};
};

inline auto CreateVmaAllocator(VmaAllocatorCreateInfo info, const GlobalDispatch& global,
                               const InstanceDispatch& instance, const DeviceDispatch& device)
    -> rstd::Result<VmaAllocatorHandle, DispatchError> {
    if (! global.vkGetInstanceProcAddr || global.vkGetInstanceProcAddr != instance.resolver ||
        ! instance.instance || ! device.device || device.instance != instance.instance ||
        ! device.vkGetDeviceProcAddr ||
        device.vkGetDeviceProcAddr != instance.vkGetDeviceProcAddr ||
        info.instance != instance.instance || info.device != device.device ||
        ! info.physicalDevice ||
        (info.vulkanApiVersion && (info.vulkanApiVersion < VK_API_VERSION_1_1 ||
                                   VK_API_VERSION_VARIANT(info.vulkanApiVersion) != 0 ||
                                   info.vulkanApiVersion > device.capabilities.api_version)) ||
        ((info.flags & VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT) &&
         ! device.capabilities.memory_budget))
        return rstd::Err(DispatchError {
            DispatchErrorKind::InvalidInput, DispatchStage::Device, "vmaCreateAllocator" });
    VmaVulkanFunctions functions {};
    functions.vkGetInstanceProcAddr = global.vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr   = device.vkGetDeviceProcAddr;
    if (! instance.vkGetPhysicalDeviceProperties)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Instance,
                                         "vkGetPhysicalDeviceProperties" });
    functions.vkGetPhysicalDeviceProperties = instance.vkGetPhysicalDeviceProperties;
    if (! instance.vkGetPhysicalDeviceMemoryProperties)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Instance,
                                         "vkGetPhysicalDeviceMemoryProperties" });
    functions.vkGetPhysicalDeviceMemoryProperties = instance.vkGetPhysicalDeviceMemoryProperties;
    if (! instance.vkGetPhysicalDeviceMemoryProperties2)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Instance,
                                         "vkGetPhysicalDeviceMemoryProperties2" });
    functions.vkGetPhysicalDeviceMemoryProperties2KHR =
        instance.vkGetPhysicalDeviceMemoryProperties2;
    if (! instance.vkGetPhysicalDeviceProperties2)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Instance,
                                         "vkGetPhysicalDeviceProperties2" });
    functions.vkGetPhysicalDeviceProperties2KHR = instance.vkGetPhysicalDeviceProperties2;
    if (! device.vkAllocateMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkAllocateMemory" });
    functions.vkAllocateMemory = device.vkAllocateMemory;
    if (! device.vkFreeMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkFreeMemory" });
    functions.vkFreeMemory = device.vkFreeMemory;
    if (! device.vkMapMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkMapMemory" });
    functions.vkMapMemory = device.vkMapMemory;
    if (! device.vkUnmapMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkUnmapMemory" });
    functions.vkUnmapMemory = device.vkUnmapMemory;
    if (! device.vkFlushMappedMemoryRanges)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkFlushMappedMemoryRanges" });
    functions.vkFlushMappedMemoryRanges = device.vkFlushMappedMemoryRanges;
    if (! device.vkInvalidateMappedMemoryRanges)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkInvalidateMappedMemoryRanges" });
    functions.vkInvalidateMappedMemoryRanges = device.vkInvalidateMappedMemoryRanges;
    if (! device.vkBindBufferMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkBindBufferMemory" });
    functions.vkBindBufferMemory = device.vkBindBufferMemory;
    if (! device.vkBindImageMemory)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkBindImageMemory" });
    functions.vkBindImageMemory = device.vkBindImageMemory;
    if (! device.vkGetBufferMemoryRequirements)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkGetBufferMemoryRequirements" });
    functions.vkGetBufferMemoryRequirements = device.vkGetBufferMemoryRequirements;
    if (! device.vkGetImageMemoryRequirements)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkGetImageMemoryRequirements" });
    functions.vkGetImageMemoryRequirements = device.vkGetImageMemoryRequirements;
    if (! device.vkCreateBuffer)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkCreateBuffer" });
    functions.vkCreateBuffer = device.vkCreateBuffer;
    if (! device.vkDestroyBuffer)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkDestroyBuffer" });
    functions.vkDestroyBuffer = device.vkDestroyBuffer;
    if (! device.vkCreateImage)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkCreateImage" });
    functions.vkCreateImage = device.vkCreateImage;
    if (! device.vkDestroyImage)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkDestroyImage" });
    functions.vkDestroyImage = device.vkDestroyImage;
    if (! device.vkCmdCopyBuffer)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkCmdCopyBuffer" });
    functions.vkCmdCopyBuffer = device.vkCmdCopyBuffer;
    if (! device.vkGetBufferMemoryRequirements2)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkGetBufferMemoryRequirements2" });
    functions.vkGetBufferMemoryRequirements2KHR = device.vkGetBufferMemoryRequirements2;
    if (! device.vkGetImageMemoryRequirements2)
        return rstd::Err(DispatchError { DispatchErrorKind::MissingCommand,
                                         DispatchStage::Device,
                                         "vkGetImageMemoryRequirements2" });
    functions.vkGetImageMemoryRequirements2KHR = device.vkGetImageMemoryRequirements2;
    if (! device.vkBindBufferMemory2)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkBindBufferMemory2" });
    functions.vkBindBufferMemory2KHR = device.vkBindBufferMemory2;
    if (! device.vkBindImageMemory2)
        return rstd::Err(DispatchError {
            DispatchErrorKind::MissingCommand, DispatchStage::Device, "vkBindImageMemory2" });
    functions.vkBindImageMemory2KHR = device.vkBindImageMemory2;
    info.pVulkanFunctions           = &functions;
    if (! info.vulkanApiVersion) info.vulkanApiVersion = device.capabilities.api_version;
    VmaAllocator allocator {};
    auto         result = vmaCreateAllocator(&info, &allocator);
    if (result != VK_SUCCESS)
        return rstd::Err(DispatchError {
            DispatchErrorKind::Vulkan, DispatchStage::Device, "vmaCreateAllocator", result });
    return rstd::Ok(VmaAllocatorHandle(allocator, 0));
}

class VmaBuffer : public Handle<VkBuffer, VmaOwner, int> {
    using Handle<VkBuffer, VmaOwner, int>::Handle;

public:
    VmaAllocation Allocation() const noexcept { return owner.allocation; }
    VkResult      MapMemory(void** data) const {
        return vmaMapMemory(owner.allocator, owner.allocation, data);
    }
    void UnMapMemory() { vmaUnmapMemory(owner.allocator, owner.allocation); }
};

class VmaImage : public Handle<VkImage, VmaOwner, int> {
    using Handle<VkImage, VmaOwner, int>::Handle;

public:
    VmaAllocation Allocation() const noexcept { return owner.allocation; }
    VkResult      MapMemory(void** data) const {
        return vmaMapMemory(owner.allocator, owner.allocation, data);
    }
    void UnMapMemory() { vmaUnmapMemory(owner.allocator, owner.allocation); }
};

constexpr inline int empty_int { 0 };

inline VkResult CreateBuffer(const VmaAllocator& vma_allocator, const VkBufferCreateInfo& ci,
                             const VmaAllocationCreateInfo& vma_info, VmaBuffer& buffer) noexcept {
    VkBuffer object;
    VmaOwner owner;
    owner.allocator = vma_allocator;

    auto res = vmaCreateBuffer(
        vma_allocator, &ci, &vma_info, &object, &owner.allocation, &owner.allocationInfo);
    if (res == VK_SUCCESS) buffer = VmaBuffer(object, owner, empty_int);
    return res;
}

inline VkResult CreateImage(const VmaAllocator& vma_allocator, const VkImageCreateInfo& ci,
                            const VmaAllocationCreateInfo& vma_info, VmaImage& vma_img) noexcept {
    VkImage  object;
    VmaOwner owner;
    owner.allocator = vma_allocator;

    auto res = vmaCreateImage(
        vma_allocator, &ci, &vma_info, &object, &owner.allocation, &owner.allocationInfo);
    if (res == VK_SUCCESS) vma_img = VmaImage(object, owner, empty_int);
    return res;
}

} // namespace vvk
