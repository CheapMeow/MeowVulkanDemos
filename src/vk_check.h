#pragma once

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

// 出错就地终止，不做任何回退
#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        VkResult vkCheckResult = (expr);                                                      \
        if (vkCheckResult != VK_SUCCESS) {                                                    \
            std::fprintf(stderr, "Vulkan call failed: %s, result %d, at %s:%d\n", #expr,             \
                         static_cast<int>(vkCheckResult), __FILE__, __LINE__);                \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)

#define FATAL(...)                                        \
    do {                                                  \
        std::fprintf(stderr, "fatal error: ");            \
        std::fprintf(stderr, __VA_ARGS__);                \
        std::fprintf(stderr, "\n");                       \
        std::abort();                                     \
    } while (0)
