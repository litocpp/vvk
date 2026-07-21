# vvk::ffi::vulkan — C++20 module wrapper for the Vulkan API.

add_library(vvk_ffi_vulkan STATIC)
add_library(vvk::ffi::vulkan ALIAS vvk_ffi_vulkan)

target_compile_features(vvk_ffi_vulkan PUBLIC cxx_std_20)
target_compile_options(vvk_ffi_vulkan PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-missing-field-initializers
)
target_sources(vvk_ffi_vulkan
    PUBLIC FILE_SET CXX_MODULES FILES
        vulkan.cppm
        constants.cppm
        types.cppm
        functions.cppm
)
target_link_libraries(vvk_ffi_vulkan
    PUBLIC
        Vulkan::Vulkan
)
set_target_properties(vvk_ffi_vulkan PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    POSITION_INDEPENDENT_CODE ON
)
