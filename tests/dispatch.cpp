#include <vulkan/vulkan.h>
#include <rstd/test/gtest.hpp>
#include <cstdlib>
#include <cstring>
#include <cstdio>
import rstd;
import vvk;
using namespace rstd::prelude;

namespace
{
template<class T>
T Fake(unsigned value) {
    return reinterpret_cast<T>(static_cast<uintptr_t>(value));
}
struct ResolverState {
    const char*           missing {};
    const char*           queries[512] {};
    unsigned              query_count {}, instance_destroyed {}, device_destroyed {}, calls[2] {};
    VkInstance            expected_instance { Fake<VkInstance>(1) };
    VkDevice              expected_device { Fake<VkDevice>(11) };
    VkResult              create_result { VK_SUCCESS };
    VkInstanceCreateFlags instance_flags {};
    bool                  queried(const char* name) const {
        for (unsigned i = 0; i < query_count; ++i)
            if (std::strcmp(queries[i], name) == 0) return true;
        return false;
    }
    bool record(const char* name) {
        if (query_count < 512) queries[query_count++] = name;
        return ! missing || std::strcmp(missing, name) != 0;
    }
} state;
void VKAPI_CALL     Uncalled() {}
VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*,
                                   VkInstance*                 out) {
    state.instance_flags = info->flags;
    if (state.create_result == VK_SUCCESS) *out = state.expected_instance;
    return state.create_result;
}
void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    EXPECT_EQ(instance, state.expected_instance);
    ++state.instance_destroyed;
}
VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                 const VkAllocationCallbacks*, VkDevice* out) {
    if (state.create_result == VK_SUCCESS) *out = state.expected_device;
    return state.create_result;
}
void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    EXPECT_EQ(device, state.expected_device);
    ++state.device_destroyed;
}
void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    out->apiVersion = VK_API_VERSION_1_3;
}
VkResult VKAPI_CALL WaitA(VkDevice device) {
    EXPECT_EQ(device, Fake<VkDevice>(11));
    ++state.calls[0];
    return VK_SUCCESS;
}
VkResult VKAPI_CALL WaitB(VkDevice device) {
    EXPECT_EQ(device, Fake<VkDevice>(22));
    ++state.calls[1];
    return VK_SUCCESS;
}
bool InstanceCommand(const char* name) {
    return std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
           std::strcmp(name, "vkEnumeratePhysicalDevices") == 0 ||
           std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0 ||
           std::strcmp(name, "vkCreateDevice") == 0 ||
           std::strcmp(name, "vkDestroyInstance") == 0 ||
           std::strcmp(name, "vkGetDeviceProcAddr") == 0 ||
           std::strcmp(name, "vkDestroySurfaceKHR") == 0 ||
           std::strstr(name, "DebugUtilsMessenger") != nullptr;
}
PFN_vkVoidFunction VKAPI_CALL DeviceResolver(VkDevice device, const char* name) {
    EXPECT_EQ(device, state.expected_device);
    EXPECT_FALSE(InstanceCommand(name));
    if (! state.record(name)) return nullptr;
    if (std::strcmp(name, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(device == Fake<VkDevice>(11) ? WaitA : WaitB);
    return Uncalled;
}
PFN_vkVoidFunction VKAPI_CALL InstanceResolver(VkInstance instance, const char* name) {
    const bool global = std::strcmp(name, "vkCreateInstance") == 0 ||
                        std::strncmp(name, "vkEnumerateInstance", 19) == 0;
    EXPECT_EQ(instance, global ? VK_NULL_HANDLE : state.expected_instance);
    if (! global) EXPECT_TRUE(InstanceCommand(name));
    if (! state.record(name)) return nullptr;
    if (std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Properties);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(DeviceResolver);
    return Uncalled;
}
VkInstanceCreateInfo InstanceInfo(VkApplicationInfo& app) {
    app            = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    return info;
}
auto Parent() -> vvk::InstanceDispatch {
    auto global = vvk::LoadGlobal(InstanceResolver).unwrap_unchecked();
    return vvk::LoadInstance(global, state.expected_instance, { .api_version = VK_API_VERSION_1_3 })
        .unwrap_unchecked();
}
int      events[8] {};
unsigned event_count {};
void     Observe(int event) {
    if (event_count < 8) events[event_count++] = event;
}
} // namespace

TEST(Dispatch, RequiredCommandsAndDomains) {
    state = {};
    EXPECT_TRUE(vvk::LoadGlobal(nullptr).is_err());
    state.missing = "vkEnumerateInstanceVersion";
    auto global   = vvk::LoadGlobal(InstanceResolver);
    ASSERT_TRUE(global.is_ok());
    auto table = global.unwrap_unchecked();
    EXPECT_EQ(table.vkEnumerateInstanceVersion, nullptr);
    auto instance = vvk::LoadInstance(table, state.expected_instance, {});
    ASSERT_TRUE(instance.is_ok());
    auto parent = instance.unwrap_unchecked();
    auto device = vvk::LoadDevice(parent, state.expected_device, {});
    ASSERT_TRUE(device.is_ok());
    auto dispatch = device.unwrap_unchecked();
    EXPECT_EQ(parent.vkDestroySurfaceKHR, nullptr);
    EXPECT_EQ(parent.vkCreateDebugUtilsMessengerEXT, nullptr);
    EXPECT_EQ(dispatch.vkQueuePresentKHR, nullptr);
    EXPECT_EQ(dispatch.vkWaitSemaphoresKHR, nullptr);
    EXPECT_EQ(dispatch.vkCmdPipelineBarrier2, nullptr);
    EXPECT_FALSE(state.queried("vkWaitSemaphores"));
    EXPECT_FALSE(state.queried("vkCreateSwapchainKHR"));
    state.missing = "vkCreateInstance";
    auto failed   = vvk::LoadGlobal(InstanceResolver);
    ASSERT_TRUE(failed.is_err());
    auto error = failed.unwrap_err_unchecked();
    EXPECT_EQ(error.stage, vvk::DispatchStage::Global);
    EXPECT_EQ(std::strcmp(error.command, "vkCreateInstance"), 0);
    state.missing     = "vkGetPhysicalDeviceProperties";
    auto bad_instance = vvk::LoadInstance(table, state.expected_instance, {});
    ASSERT_TRUE(bad_instance.is_err());
    EXPECT_EQ(bad_instance.unwrap_err_unchecked().stage, vvk::DispatchStage::Instance);
    state.missing   = "vkAllocateMemory";
    auto bad_device = vvk::LoadDevice(parent, state.expected_device, {});
    ASSERT_TRUE(bad_device.is_err());
    EXPECT_EQ(std::strcmp(bad_device.unwrap_err_unchecked().command, "vkAllocateMemory"), 0);
}

TEST(Dispatch, VersionFeaturesAndLegalAliases) {
    state                                            = {};
    auto                                      parent = Parent();
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES
    };
    timeline.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features sync {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES
    };
    sync.synchronization2 = VK_TRUE;
    timeline.pNext        = &sync;
    VkDeviceCreateInfo info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.pNext = &timeline;
    auto caps  = vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_3);
    ASSERT_TRUE(caps.is_ok());
    auto core = vvk::LoadDevice(parent, state.expected_device, caps.unwrap_unchecked());
    ASSERT_TRUE(core.is_ok());
    EXPECT_TRUE(state.queried("vkWaitSemaphores"));
    EXPECT_TRUE(state.queried("vkCmdPipelineBarrier2"));
    EXPECT_FALSE(state.queried("vkWaitSemaphoresKHR"));
    EXPECT_FALSE(state.queried("vkCmdPipelineBarrier2KHR"));
    parent.capabilities.api_version = VK_API_VERSION_1_1;
    EXPECT_TRUE(
        vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_3).is_err());
    const char* names[]          = { VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                                     VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME };
    info.enabledExtensionCount   = 2;
    info.ppEnabledExtensionNames = names;
    auto parsed = vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_3);
    ASSERT_TRUE(parsed.is_ok());
    state.query_count = 0;
    auto extension    = vvk::LoadDevice(parent, state.expected_device, parsed.unwrap_unchecked());
    ASSERT_TRUE(extension.is_ok());
    EXPECT_TRUE(state.queried("vkWaitSemaphoresKHR"));
    EXPECT_TRUE(state.queried("vkCmdPipelineBarrier2KHR"));
    EXPECT_FALSE(state.queried("vkWaitSemaphores"));
    EXPECT_FALSE(state.queried("vkCmdPipelineBarrier2"));
    state.missing = "vkWaitSemaphoresKHR";
    EXPECT_TRUE(vvk::LoadDevice(parent, state.expected_device, parsed.unwrap_unchecked()).is_err());
    info.pNext    = nullptr;
    auto disabled = vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_3)
                        .unwrap_unchecked();
    EXPECT_FALSE(disabled.timeline_semaphore);
    EXPECT_FALSE(disabled.synchronization2);
    const char* swapchain        = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = &swapchain;
    EXPECT_TRUE(
        vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_3).is_err());
    auto invalid        = disabled;
    invalid.api_version = VK_API_VERSION_1_3;
    EXPECT_TRUE(vvk::LoadDevice(parent, state.expected_device, invalid).is_err());
    invalid                    = disabled;
    invalid.timeline_extension = false;
    invalid.timeline_semaphore = true;
    EXPECT_TRUE(vvk::LoadDevice(parent, state.expected_device, invalid).is_err());
    VkApplicationInfo app;
    auto              instance_info = InstanceInfo(app);
    app.apiVersion                  = VK_API_VERSION_1_0;
    EXPECT_TRUE(vvk::ParseInstanceCapabilities(instance_info).is_err());
}

TEST(Dispatch, IndependentDevices) {
    state             = {};
    auto first_parent = Parent();
    auto first        = vvk::LoadDevice(first_parent, state.expected_device, {}).unwrap_unchecked();
    state.expected_instance = Fake<VkInstance>(2);
    state.expected_device   = Fake<VkDevice>(22);
    auto second_parent      = Parent();
    auto second = vvk::LoadDevice(second_parent, state.expected_device, {}).unwrap_unchecked();
    EXPECT_NE(first.vkDeviceWaitIdle, second.vkDeviceWaitIdle);
    EXPECT_EQ(first.vkDeviceWaitIdle(first.device), VK_SUCCESS);
    EXPECT_EQ(second.vkDeviceWaitIdle(second.device), VK_SUCCESS);
    EXPECT_EQ(first.vkDeviceWaitIdle(first.device), VK_SUCCESS);
    EXPECT_EQ(state.calls[0], 2u);
    EXPECT_EQ(state.calls[1], 1u);
}

TEST(Dispatch, CreationRollbackAndApiErrors) {
    state                        = {};
    auto                  global = vvk::LoadGlobal(InstanceResolver).unwrap_unchecked();
    VkApplicationInfo     app;
    auto                  info = InstanceInfo(app);
    vvk::InstanceDispatch parent;
    vvk::Instance         instance;
    state.missing = "vkGetPhysicalDeviceProperties";
    auto failed   = vvk::Instance::Create(instance, global, info, parent);
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(state.instance_destroyed, 1u);
    EXPECT_FALSE(bool(instance));
    EXPECT_EQ(parent.instance, VK_NULL_HANDLE);
    EXPECT_EQ(parent.vkDestroyInstance, nullptr);
    state.missing       = nullptr;
    state.create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    auto api_error      = vvk::Instance::Create(instance, global, info, parent);
    ASSERT_TRUE(api_error.is_err());
    EXPECT_EQ(api_error.unwrap_err_unchecked().api_result, VK_ERROR_OUT_OF_HOST_MEMORY);
    EXPECT_EQ(state.instance_destroyed, 1u);
    state.create_result          = VK_SUCCESS;
    const char* portability      = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = &portability;
    ASSERT_TRUE(vvk::Instance::Create(instance, global, info, parent).is_ok());
    EXPECT_TRUE((state.instance_flags & VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR) != 0);
    vvk::DeviceDispatch table;
    vvk::Device         device;
    VkDeviceCreateInfo  device_info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    state.missing = "vkCreateBuffer";
    auto device_failed =
        vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, table);
    ASSERT_TRUE(device_failed.is_err());
    EXPECT_EQ(state.device_destroyed, 1u);
    EXPECT_FALSE(bool(device));
    EXPECT_EQ(table.device, VK_NULL_HANDLE);
    EXPECT_EQ(table.vkDestroyDevice, nullptr);
    state.missing       = nullptr;
    state.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto device_api =
        vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, table);
    ASSERT_TRUE(device_api.is_err());
    EXPECT_EQ(device_api.unwrap_err_unchecked().api_result, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_EQ(state.device_destroyed, 1u);
    state.create_result = VK_SUCCESS;
    ASSERT_TRUE(
        vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, table).is_ok());
    EXPECT_TRUE(vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, table)
                    .is_err());
    device.reset();
    instance.reset();
    EXPECT_EQ(state.device_destroyed, 2u);
    EXPECT_EQ(state.instance_destroyed, 2u);
}

TEST(Dispatch, MalformedResolverPreservesUnownedHandles) {
    state                        = {};
    auto                  global = vvk::LoadGlobal(InstanceResolver).unwrap_unchecked();
    auto                  parent = Parent();
    VkApplicationInfo     app;
    auto                  info = InstanceInfo(app);
    vvk::Instance         instance;
    vvk::InstanceDispatch table;
    state.missing = "vkDestroyInstance";
    auto failed   = vvk::Instance::Create(instance, global, info, table);
    ASSERT_TRUE(failed.is_err());
    auto error = failed.unwrap_err_unchecked();
    EXPECT_EQ(error.unowned_instance, state.expected_instance);
    EXPECT_EQ(state.instance_destroyed, 0u);
    DestroyInstance(error.unowned_instance, nullptr);
    state.missing = "vkDestroyDevice";
    VkDeviceCreateInfo  device_info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    vvk::Device         device;
    vvk::DeviceDispatch device_table;
    auto                failed_device =
        vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, device_table);
    ASSERT_TRUE(failed_device.is_err());
    auto device_error = failed_device.unwrap_err_unchecked();
    EXPECT_EQ(device_error.unowned_device, state.expected_device);
    EXPECT_EQ(state.device_destroyed, 0u);
    DestroyDevice(device_error.unowned_device, nullptr);
}

TEST(Dispatch, ConsumerFactoriesAndMemoryValidation) {
    state       = {};
    auto parent = Parent();
    auto device = vvk::LoadDevice(parent, state.expected_device, {}).unwrap_unchecked();
    auto memory = vvk::MemoryDispatch::FromDispatch(parent, device);
    EXPECT_TRUE(memory.valid());
    EXPECT_EQ(memory.properties, parent.vkGetPhysicalDeviceProperties);
    auto descriptors = vvk::DescriptorDeviceDispatch::FromDispatch(device);
    EXPECT_TRUE(descriptors.valid());
    auto timeline = vvk::TimelineSemaphoreDeviceDispatch::FromDispatch(device);
    EXPECT_FALSE(timeline.valid());
    auto broken     = device;
    broken.instance = Fake<VkInstance>(9);
    EXPECT_TRUE(vvk::MemoryAllocator::Create(Fake<VkPhysicalDevice>(3), parent, broken).is_err());
}

TEST(Loader, MissingLibraryAndInjectedMove) {
    state        = {};
    auto missing = vvk::VulkanLoader::Open(
        rstd::ffi::CStr::from_ptr("/vvk-nonexistent-directory/libvulkan.so.1"));
    ASSERT_TRUE(missing.is_err());
    auto error = missing.unwrap_err_unchecked();
    EXPECT_EQ(error.kind, vvk::LoaderErrorKind::OpenLibrary);
    EXPECT_FALSE(error.message.is_empty());
    if (std::getenv("VVK_TEST_EXPECT_NO_LOADER")) {
        auto system = vvk::VulkanLoader::Open();
        ASSERT_TRUE(system.is_err());
        EXPECT_EQ(system.unwrap_err_unchecked().kind, vvk::LoaderErrorKind::OpenLibrary);
    }
    EXPECT_TRUE(vvk::VulkanLoader::FromResolver(nullptr).is_err());
    auto loaded = vvk::VulkanLoader::FromResolver(InstanceResolver);
    ASSERT_TRUE(loaded.is_ok());
    auto  original = rstd::move(loaded).unwrap_unchecked();
    auto* table    = &original.global();
    auto  moved    = rstd::move(original);
    EXPECT_EQ(&moved.global(), table);
    EXPECT_EQ(table->vkGetInstanceProcAddr, InstanceResolver);
}

TEST(Loader, SharedLibraryLifetime) {
    const char* path         = std::getenv("VVK_TEST_LOADER");
    const char* missing_path = std::getenv("VVK_TEST_MISSING_ROOT");
    if (! path || ! missing_path)
        GTEST_SKIP() << "Run tests/verify-loader.sh to build and exercise the test libraries";
    auto missing = vvk::VulkanLoader::Open(rstd::ffi::CStr::from_ptr(missing_path));
    ASSERT_TRUE(missing.is_err());
    auto error = missing.unwrap_err_unchecked();
    EXPECT_EQ(error.kind, vvk::LoaderErrorKind::RootSymbol);
    EXPECT_FALSE(error.message.is_empty());
    event_count = 0;
    {
        auto opened = vvk::VulkanLoader::Open(rstd::ffi::CStr::from_ptr(path));
        ASSERT_TRUE(opened.is_ok());
        auto  original     = rstd::move(opened).unwrap_unchecked();
        auto* global       = &original.global();
        auto  set_observer = reinterpret_cast<void (*)(void (*)(int))>(
            global->vkGetInstanceProcAddr(VK_NULL_HANDLE, "vvkTestSetObserver"));
        ASSERT_NE(set_observer, nullptr);
        set_observer(Observe);
        auto loader = rstd::move(original);
        EXPECT_EQ(global, &loader.global());
        vvk::InstanceDispatch parent;
        vvk::DeviceDispatch   table;
        vvk::Instance         instance;
        vvk::Device           device;
        VkApplicationInfo     app;
        auto                  info = InstanceInfo(app);
        ASSERT_TRUE(vvk::Instance::Create(instance, *global, info, parent).is_ok());
        VkDeviceCreateInfo device_info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        ASSERT_TRUE(
            vvk::Device::Create(device, Fake<VkPhysicalDevice>(3), parent, device_info, table)
                .is_ok());
        EXPECT_EQ(event_count, 2u);
    }
    ASSERT_EQ(event_count, 5u);
    for (unsigned i = 0; i < 5; ++i) EXPECT_EQ(events[i], int(i + 1));
}

TEST(Dispatch, ImageFormatListCapability) {
    vvk::InstanceCapabilities parent;
    VkDeviceCreateInfo        info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    auto                      parse = [&](unsigned physical) {
        return vvk::ParseDeviceCapabilities(info, parent, physical).unwrap_unchecked();
    };
    EXPECT_FALSE(parse(VK_API_VERSION_1_2).image_format_list);
    parent.api_version = VK_API_VERSION_1_2;
    EXPECT_TRUE(parse(VK_API_VERSION_1_2).image_format_list);
    EXPECT_FALSE(parse(VK_API_VERSION_1_1).image_format_list);
    parent.api_version           = VK_API_VERSION_1_1;
    const char* extension        = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = &extension;
    EXPECT_TRUE(parse(VK_API_VERSION_1_1).image_format_list);
}

TEST(Dispatch, BufferAddressFeaturesAndAliases) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        state                           = {};
        auto parent                     = Parent();
        parent.capabilities.api_version = mode == 1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2;
        VkDeviceCreateInfo                          info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkPhysicalDeviceBufferDeviceAddressFeatures feature {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
        };
        feature.bufferDeviceAddress = mode != 2;
        info.pNext                  = &feature;
        const char* extension       = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        if (mode == 1) {
            info.enabledExtensionCount   = 1;
            info.ppEnabledExtensionNames = &extension;
        }
        auto parsed = vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_2);
        ASSERT_TRUE(parsed.is_ok());
        auto caps = parsed.unwrap_unchecked();
        EXPECT_EQ(caps.buffer_device_address, mode != 2);
        auto loaded = vvk::LoadDevice(parent, state.expected_device, caps);
        ASSERT_TRUE(loaded.is_ok());
        EXPECT_EQ(state.queried("vkGetBufferDeviceAddress"), mode == 0);
        EXPECT_EQ(state.queried("vkGetBufferDeviceAddressKHR"), mode == 1);
        EXPECT_FALSE(state.queried("vkGetBufferDeviceAddressEXT"));
        if (mode != 2) {
            state.missing = mode == 0 ? "vkGetBufferDeviceAddress" : "vkGetBufferDeviceAddressKHR";
            auto missing  = vvk::LoadDevice(parent, state.expected_device, caps);
            ASSERT_TRUE(missing.is_err());
            EXPECT_EQ(missing.unwrap_err_unchecked().kind, vvk::DispatchErrorKind::MissingCommand);
            vvk::Device         owner;
            vvk::DeviceDispatch table;
            auto                created =
                vvk::Device::Create(owner, Fake<VkPhysicalDevice>(3), parent, info, table);
            EXPECT_TRUE(created.is_err());
            EXPECT_EQ(state.device_destroyed, 1u);
        }
    }
    VkDeviceCreateInfo               info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceVulkan12Features feature {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };
    feature.bufferDeviceAddress = VK_TRUE;
    info.pNext                  = &feature;
    vvk::InstanceCapabilities parent { .api_version = VK_API_VERSION_1_2 };
    EXPECT_TRUE(vvk::ParseDeviceCapabilities(info, parent, VK_API_VERSION_1_2)
                    .unwrap_unchecked()
                    .buffer_device_address);
    EXPECT_TRUE(vvk::ParseDeviceCapabilities(info, parent, VK_API_VERSION_1_1).is_err());
    VkDeviceGroupDeviceCreateInfo group { VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO };
    group.physicalDeviceCount = 2;
    feature.pNext             = &group;
    EXPECT_TRUE(vvk::ParseDeviceCapabilities(info, parent, VK_API_VERSION_1_2).is_err());
    VkPhysicalDeviceBufferDeviceAddressFeatures duplicate {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    feature.pNext = &duplicate;
    EXPECT_TRUE(vvk::ParseDeviceCapabilities(info, parent, VK_API_VERSION_1_2).is_err());
}

TEST(Dispatch, BufferAddressExtensionDoesNotEnableFeature) {
    state                           = {};
    auto parent                     = Parent();
    parent.capabilities.api_version = VK_API_VERSION_1_1;
    VkDeviceCreateInfo info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    const char*        extension = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = &extension;
    auto parsed = vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_1);
    ASSERT_TRUE(parsed.is_ok());
    auto caps = parsed.unwrap_unchecked();
    EXPECT_FALSE(caps.buffer_device_address);
    EXPECT_TRUE(vvk::LoadDevice(parent, state.expected_device, caps).is_ok());
    EXPECT_FALSE(state.queried("vkGetBufferDeviceAddressKHR"));
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT old {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT
    };
    old.bufferDeviceAddress = VK_TRUE;
    extension               = VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    info.pNext              = &old;
    EXPECT_FALSE(vvk::ParseDeviceCapabilities(info, parent.capabilities, VK_API_VERSION_1_1)
                     .unwrap_unchecked()
                     .buffer_device_address);
}
