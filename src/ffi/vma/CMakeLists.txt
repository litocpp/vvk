# vvk::ffi::vma — C++20 module wrapper for VulkanMemoryAllocator.

add_library(vvk_ffi_vma STATIC vma.cpp)
add_library(vvk::ffi::vma ALIAS vvk_ffi_vma)

target_compile_features(vvk_ffi_vma PUBLIC cxx_std_20)
target_compile_options(vvk_ffi_vma PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-Wno-nullability-completeness>
    $<$<CXX_COMPILER_ID:Clang>:-Wno-nullability-extension>
    $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-private-field>
    $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-parameter>
    $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-variable>
)
target_sources(vvk_ffi_vma
    PUBLIC FILE_SET CXX_MODULES FILES
        vma.cppm
)
target_link_libraries(vvk_ffi_vma
    PUBLIC
        GPUOpen::VulkanMemoryAllocator
        vvk::ffi::vulkan
)
set_target_properties(vvk_ffi_vma PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    POSITION_INDEPENDENT_CODE ON
)
