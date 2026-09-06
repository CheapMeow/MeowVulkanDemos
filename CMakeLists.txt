cmake_minimum_required(VERSION 3.16)
project(VulkanIndirectDrawDemo C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "生成 compile_commands.json" FORCE)

if(MSVC)
    add_compile_options(/W3)
endif()

set(VULKAN_SDK_DIR "D:/path/to/VulkanSDK" CACHE PATH "Vulkan SDK 安装目录")
find_program(GLSLC_EXECUTABLE NAMES glslc glslc.exe
    HINTS "${VULKAN_SDK_DIR}/Bin"
    DOC "glslc 着色器编译器路径")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
set(IMPLOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/implot")

set(SHADER_SOURCES
    shaders/gbuffer.vert
    shaders/gbuffer.frag
    shaders/fullscreen.vert
    shaders/lighting.frag
    shaders/cull.comp
)

# 两个平台共用的源码：场景数据、Vulkan 资源、渲染器、耗时统计、界面
set(DEMO_COMMON_SOURCES
    src/asset_file.cpp
    src/control_server.cpp
    src/obj_loader.cpp
    src/rdoc_trigger.cpp
    src/scene.cpp
    src/timing.cpp
    src/vk_context.cpp
    src/vk_marker.cpp
    src/vk_resources.cpp
    src/renderer.cpp
    src/user_interface.cpp
    src/frame_capture.cpp
    src/stb_image_impl.cpp
    src/stb_image_write_impl.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    "${IMPLOT_DIR}/implot.cpp"
    "${IMPLOT_DIR}/implot_items.cpp"
)

set(DEMO_INCLUDE_DIRS
    external
    external/glm
    external/stb
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${IMPLOT_DIR}"
    src
)

set(DEMO_COMPILE_DEFINITIONS
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(ANDROID)
    # 安卓上不需要窗口库，也没有 main 函数入口，构建目标是动态库，入口由 android_main 提供。
    # 编译产物打进 APK，着色器统一用 1.1，覆盖只支持 1.1 的设备
    add_library(VulkanIndirectDrawDemo SHARED
        ${DEMO_COMMON_SOURCES}
        src/android_main.cpp
        src/gpu_clock_lock.cpp
        "${IMGUI_DIR}/backends/imgui_impl_android.cpp"
        "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
    )

    # AGP 把 src/main/assets 整个打进 APK，编译出来的 .spv 直接写进这个目录，
    # 运行时的名字与桌面端一致，都是 shaders/xxx.spv
    set(ASSET_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/app/src/main/assets")
    file(MAKE_DIRECTORY "${ASSET_ROOT_DIR}")
    set(SHADER_BINARY_DIR "${ASSET_ROOT_DIR}/shaders")
    set(SHADER_TARGET_ENV vulkan1.1)

    target_include_directories(VulkanIndirectDrawDemo PRIVATE
        ${DEMO_INCLUDE_DIRS}
        "${ANDROID_NDK}/sources/android/native_app_glue"
    )

    target_compile_options(VulkanIndirectDrawDemo PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )

    target_compile_definitions(VulkanIndirectDrawDemo PRIVATE
        ${DEMO_COMPILE_DEFINITIONS}
        ASSET_ROOT_DIR="${ASSET_ROOT_DIR}"
        # NDK 的 Vulkan 头文件默认不含安卓表面类型，必须显式声明平台
        VK_USE_PLATFORM_ANDROID_KHR
    )

    target_link_libraries(VulkanIndirectDrawDemo PRIVATE
        vulkan
        android
        log
    )
else()
    # 桌面端：GLFW 提供窗口与输入，着色器输出到构建目录，代码以绝对路径引用构建产物
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/glfw)

    add_executable(VulkanIndirectDrawDemo
        ${DEMO_COMMON_SOURCES}
        src/main.cpp
        src/console.cpp
        src/gpu_clock_lock.cpp
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    )

    file(COPY assets/backpack DESTINATION "${CMAKE_BINARY_DIR}")

    set(ASSET_ROOT_DIR "${CMAKE_BINARY_DIR}")
    set(SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
    set(SHADER_TARGET_ENV vulkan1.2)

    target_include_directories(VulkanIndirectDrawDemo PRIVATE
        "${VULKAN_SDK_DIR}/Include"
        ${DEMO_INCLUDE_DIRS}
    )

    if(MSVC)
        target_compile_options(VulkanIndirectDrawDemo PRIVATE /utf-8 /wd4819)
    endif()

    target_compile_definitions(VulkanIndirectDrawDemo PRIVATE
        ${DEMO_COMPILE_DEFINITIONS}
        GLFW_INCLUDE_VULKAN
        _CRT_SECURE_NO_WARNINGS
        ASSET_ROOT_DIR="${ASSET_ROOT_DIR}"
    )

    target_link_libraries(VulkanIndirectDrawDemo PRIVATE
        glfw
        "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib"
    )
endif()

file(MAKE_DIRECTORY "${SHADER_BINARY_DIR}")

set(COMPILED_SHADERS)
foreach(SHADER_SOURCE ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
    set(SHADER_OUTPUT "${SHADER_BINARY_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=${SHADER_TARGET_ENV}
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
