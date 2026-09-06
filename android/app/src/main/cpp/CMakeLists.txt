cmake_minimum_required(VERSION 3.22.1)
project(VulkanIndirectDrawDemoAndroid C CXX)

# AGP 要求 CMakeLists 在模块目录内。仓库根目录的 CMakeLists 在 ANDROID 分支里
# 定义全部源码，这里把它作为子目录加进来，构建产物放在独立目录
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." "${CMAKE_BINARY_DIR}/repo_build")
