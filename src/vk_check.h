#pragma once

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

// 出错就地终止，不做任何回退
#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        VkResult vkCheckResult = (expr);                                                      \
        if (vkCheckResult != VK_SUCCESS) {                                                    \
            std::fprintf(stderr, "Vulkan 调用失败: %s, 返回值 %d, 位置 %s:%d\n", #expr,       \
                         static_cast<int>(vkCheckResult), __FILE__, __LINE__);                \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)

#define FATAL(...)                                        \
    do {                                                  \
        std::fprintf(stderr, "致命错误: ");               \
        std::fprintf(stderr, __VA_ARGS__);                \
        std::fprintf(stderr, "\n");                       \
        std::abort();                                     \
    } while (0)
