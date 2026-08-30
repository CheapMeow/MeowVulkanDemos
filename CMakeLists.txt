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
    src/vk_resources.cpp
    src/obj_loader.cpp
    src/scene.cpp
    src/renderer.cpp
    src/frame_capture.cpp
    src/stb_image_impl.cpp
    src/stb_image_write_impl.cpp
)

set(SHADER_SOURCES
    shaders/gbuffer.vert
    shaders/gbuffer.frag
    shaders/fullscreen.vert
    shaders/lighting.frag
)

set(SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_BINARY_DIR}")

set(COMPILED_SHADERS)
foreach(SHADER_SOURCE ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
    set(SHADER_OUTPUT "${SHADER_BINARY_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.2
                -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
                "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SOURCE}" -o "${SHADER_OUTPUT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SOURCE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_common.glsl"
        COMMENT "编译着色器 ${SHADER_NAME}"
        VERBATIM
    )
    list(APPEND COMPILED_SHADERS "${SHADER_OUTPUT}")
endforeach()

add_custom_target(CompileShaders DEPENDS ${COMPILED_SHADERS})
add_dependencies(VulkanIndirectDrawDemo CompileShaders)

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
    _CRT_SECURE_NO_WARNINGS
    PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SHADER_BINARY_DIR="${SHADER_BINARY_DIR}"
)
