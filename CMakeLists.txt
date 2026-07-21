cmake_minimum_required(VERSION 3.28)

project(
    vvk
    VERSION 0.1.0
    DESCRIPTION "Vulkan object and dispatch helpers for C++20"
    LANGUAGES CXX
)

option(VVK_FETCH_MISSING_DEPENDENCIES
    "Fetch rstd and VulkanMemoryAllocator when their targets are unavailable"
    ${PROJECT_IS_TOP_LEVEL}
)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(Vulkan REQUIRED)
include(FetchContent)

if(NOT TARGET rstd::rstd)
    if(NOT VVK_FETCH_MISSING_DEPENDENCIES)
        message(FATAL_ERROR
            "vvk requires rstd::rstd, rstd.cppstd, and rstd.log targets")
    endif()

    set(RSTD_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RSTD_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(RSTD_BENCHMARKS_IN_CTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        rstd
        GIT_REPOSITORY https://github.com/litocpp/rstd.git
        GIT_TAG e1e8089c64a93b5349dcf822b27e6cb8a8b32f2f
        GIT_SHALLOW FALSE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(rstd)
endif()

foreach(_vvk_rstd_target IN ITEMS rstd::rstd rstd.cppstd rstd.log)
    if(NOT TARGET ${_vvk_rstd_target})
        message(FATAL_ERROR "vvk requires the ${_vvk_rstd_target} target")
    endif()
endforeach()

if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
    if(NOT VVK_FETCH_MISSING_DEPENDENCIES)
        message(FATAL_ERROR
            "vvk requires the GPUOpen::VulkanMemoryAllocator target")
    endif()

    set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
    set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG 3aa921224c154a0d2c43912bc88e1c42ce1f7607
        GIT_SHALLOW FALSE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(vma)
endif()

add_subdirectory(src/ffi/vulkan)
add_subdirectory(src/ffi/vma)

add_library(vvk STATIC src/vvk.cpp)
add_library(vvk::vvk ALIAS vvk)

target_compile_features(vvk PUBLIC cxx_std_20)
target_compile_options(vvk PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-missing-field-initializers
)
target_sources(vvk
    PUBLIC FILE_SET CXX_MODULES FILES
        src/vvk.cppm
        src/handle.cppm
        src/vma.cppm
        src/dispatch.cppm
        src/objects.cppm
        src/completion.cppm
        src/descriptor.cppm
    PUBLIC FILE_SET public_headers TYPE HEADERS
        BASE_DIRS include
        FILES include/vvk/macros.hpp
)
target_link_libraries(vvk
    PUBLIC
        rstd::rstd
        rstd.cppstd
        vvk::ffi::vulkan
        vvk::ffi::vma
    PRIVATE
        rstd.log
)
set_target_properties(vvk PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    POSITION_INDEPENDENT_CODE ON
)
