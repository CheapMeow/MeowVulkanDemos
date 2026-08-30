cmake_minimum_required(VERSION 3.16)
project(VulkanIndirectDrawDemo C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
endif()

set(VULKAN_SDK_DIR "D:/path/to/VulkanSDK" CACHE PATH "Vulkan SDK 安装目录")
set(GLSLC_EXECUTABLE "${VULKAN_SDK_DIR}/Bin/glslc.exe")

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

add_executable(VulkanIndirectDrawDemo
    src/main.cpp
    src/vk_context.cpp
)

target_include_directories(VulkanIndirectDrawDemo PRIVATE
    "${VULKAN_SDK_DIR}/Include"
    external/glm
    external/stb
    src
)

target_link_libraries(VulkanIndirectDrawDemo PRIVATE
    glfw
    "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib"
)

if(MSVC)
    target_compile_options(VulkanIndirectDrawDemo PRIVATE /utf-8 /wd4819)
endif()

target_compile_definitions(VulkanIndirectDrawDemo PRIVATE
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
