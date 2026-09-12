cmake_minimum_required(VERSION 3.16)
project(MeowVulkanDemos C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "生成 compile_commands.json" FORCE)

if(MSVC)
    # 源码统一是 UTF-8，MSVC 默认按系统代码页解释，必须显式指定，否则中文文本会被误读
    add_compile_options(/W3 /utf-8 /wd4819)
endif()

# 一次只构建一个 case。桌面构建脚本与安卓 Gradle 都通过 -DDEMO_CASE=... 传入名字，
# 对应 cases/<名字> 这个目录
set(DEMO_CASE "indirect_draw" CACHE STRING "要构建的 case 名字")
set(DEMO_CASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cases/${DEMO_CASE}")
if(NOT EXISTS "${DEMO_CASE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "unknown case '${DEMO_CASE}': no cases/${DEMO_CASE}/CMakeLists.txt")
endif()

# Vulkan SDK 的路径不写进仓库。桌面构建需要它的头文件、库与 glslc，全部由环境变量
# VULKAN_SDK_DIR 提供：scripts\local_env.bat（不入库）里 set 好，scripts\build.bat 执行它
# 之后 CMake 在这里只从环境读取。安卓构建不需要头文件与库（由 NDK 提供），但编译着色器
# 仍然需要 glslc，它经 VULKAN_SDK_DIR 或 PATH 找到
set(VULKAN_SDK_DIR "$ENV{VULKAN_SDK_DIR}")
if(VULKAN_SDK_DIR)
    find_program(GLSLC_EXECUTABLE NAMES glslc glslc.exe
        HINTS "${VULKAN_SDK_DIR}/Bin"
        DOC "glslc 着色器编译器路径")
else()
    find_program(GLSLC_EXECUTABLE NAMES glslc glslc.exe
        DOC "glslc 着色器编译器路径")
endif()
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found: set VULKAN_SDK_DIR in scripts\\local_env.bat or add glslc to PATH and rerun")
endif()

set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(IMGUI_DIR "${EXTERNAL_DIR}/imgui")
set(IMPLOT_DIR "${EXTERNAL_DIR}/implot")
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")

# 资源根目录与着色器输出位置按平台区分：
# 桌面把编译目录当资源根，着色器写进 build/shaders；
# 安卓把 APK 的 assets 目录当资源根，着色器直接写进工程的 assets，由 AGP 打进 APK
if(ANDROID)
    set(DEMO_ASSET_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/app/src/main/assets")
    file(MAKE_DIRECTORY "${DEMO_ASSET_ROOT_DIR}")
    set(DEMO_SHADER_OUTPUT_DIR "${DEMO_ASSET_ROOT_DIR}/shaders")
    # 安卓统一用 1.1，覆盖只支持 1.1 的设备
    set(DEMO_SHADER_TARGET_ENV vulkan1.1)
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/backpack")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/backpack" DESTINATION "${CMAKE_BINARY_DIR}")
    endif()
    set(DEMO_ASSET_ROOT_DIR "${CMAKE_BINARY_DIR}")
    set(DEMO_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    set(DEMO_SHADER_TARGET_ENV vulkan1.2)
endif()
file(MAKE_DIRECTORY "${DEMO_SHADER_OUTPUT_DIR}")

# 全部 case 共用的源码：Vulkan 上下文与资源、资源读取、模型解析、场景与相机、
# 计时、界面公共部分、控制服务、抓帧、锁频、控制台
set(DEMO_COMMON_SOURCES
    "${COMMON_DIR}/src/asset_file.cpp"
    "${COMMON_DIR}/src/control_server.cpp"
    "${COMMON_DIR}/src/frame_capture.cpp"
    "${COMMON_DIR}/src/gpu_clock_lock.cpp"
    "${COMMON_DIR}/src/obj_loader.cpp"
    "${COMMON_DIR}/src/scene.cpp"
    "${COMMON_DIR}/src/timing.cpp"
    "${COMMON_DIR}/src/user_interface.cpp"
    "${COMMON_DIR}/src/vk_context.cpp"
    "${COMMON_DIR}/src/vk_resources.cpp"
    "${COMMON_DIR}/src/stb_image_impl.cpp"
    "${COMMON_DIR}/src/stb_image_write_impl.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
    "${IMPLOT_DIR}/implot.cpp"
    "${IMPLOT_DIR}/implot_items.cpp"
)

set(DEMO_COMMON_INCLUDE_DIRS
    "${COMMON_DIR}/include"
    "${EXTERNAL_DIR}"
    "${EXTERNAL_DIR}/glm"
    "${EXTERNAL_DIR}/stb"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${IMPLOT_DIR}"
)

if(ANDROID)
    list(APPEND DEMO_COMMON_SOURCES "${IMGUI_DIR}/backends/imgui_impl_android.cpp")
else()
    list(APPEND DEMO_COMMON_SOURCES
        "${COMMON_DIR}/src/console.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    )
endif()

add_library(demo_common STATIC ${DEMO_COMMON_SOURCES})

target_include_directories(demo_common PUBLIC ${DEMO_COMMON_INCLUDE_DIRS})
target_compile_definitions(demo_common PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    ASSET_ROOT_DIR="${DEMO_ASSET_ROOT_DIR}"
)

if(ANDROID)
    # NDK 的 Vulkan 头文件默认不含安卓表面类型，必须显式声明平台
    target_compile_definitions(demo_common PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
    target_compile_options(demo_common PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(demo_common PUBLIC vulkan android log)
else()
    # 桌面构建必须有 Vulkan SDK：头文件、vulkan-1.lib 与 glslc 都在它下面
    if(NOT VULKAN_SDK_DIR)
        message(FATAL_ERROR "VULKAN_SDK_DIR is not set: point it at the Vulkan SDK in scripts\\local_env.bat and rerun")
    endif()
    if(NOT EXISTS "${VULKAN_SDK_DIR}/Include/vulkan/vulkan.h")
        message(FATAL_ERROR "VULKAN_SDK_DIR does not contain a Vulkan SDK: ${VULKAN_SDK_DIR}")
    endif()

    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${EXTERNAL_DIR}/glfw")

    target_include_directories(demo_common PUBLIC "${VULKAN_SDK_DIR}/Include")
    target_compile_definitions(demo_common PUBLIC
        GLFW_INCLUDE_VULKAN
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(demo_common PUBLIC
        glfw
        "${VULKAN_SDK_DIR}/Lib/vulkan-1.lib"
    )
endif()

# 注册一个 case。case 目录的 CMakeLists 只需给出自己的源码、入口与着色器，
# 平台差异、包含目录、链接库与着色器编译都在这里统一处理
function(meow_add_case case_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DESKTOP_ENTRY;ANDROID_ENTRY;SHADERS;SHADER_INCLUDES" ${ARGN})

    if(ANDROID)
        # 安卓上不需要窗口库，入口是 android_main，构建目标是动态库，名字固定，
        # 与 AndroidManifest 里的 android.app.lib_name 一致
        add_library(meow_vulkan_demo SHARED
            ${ARG_SOURCES}
            ${ARG_ANDROID_ENTRY}
            "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
        )
        set(case_target meow_vulkan_demo)
        target_include_directories(${case_target} PRIVATE
            "${ANDROID_NDK}/sources/android/native_app_glue"
        )
        target_compile_options(${case_target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    else()
        add_executable(meow_${case_name} ${ARG_SOURCES} ${ARG_DESKTOP_ENTRY})
        set_target_properties(meow_${case_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set(case_target meow_${case_name})
    endif()

    target_link_libraries(${case_target} PRIVATE demo_common)

    # 把 case 自己的 GLSL 编译成 SPIR-V，输出目录由平台决定，运行时的名字都是 shaders/xxx.spv
    set(compiled_shaders)
    foreach(shader_source ${ARG_SHADERS})
        get_filename_component(shader_name "${shader_source}" NAME)
        set(shader_output "${DEMO_SHADER_OUTPUT_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=${DEMO_SHADER_TARGET_ENV}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
                    "${CMAKE_CURRENT_SOURCE_DIR}/${shader_source}" -o "${shader_output}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader_source}"
                    ${ARG_SHADER_INCLUDES}
            COMMENT "编译着色器 ${shader_name}"
            VERBATIM
        )
        list(APPEND compiled_shaders "${shader_output}")
    endforeach()

    add_custom_target(${case_name}_shaders DEPENDS ${compiled_shaders})
    add_dependencies(${case_target} ${case_name}_shaders)
endfunction()

add_subdirectory("${DEMO_CASE_DIR}" "${CMAKE_BINARY_DIR}/case")
