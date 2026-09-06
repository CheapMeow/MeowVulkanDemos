#pragma once

#include <vulkan/vulkan.h>

#ifndef __ANDROID__
#include <GLFW/glfw3.h>
#else
struct ANativeWindow;
#endif

#include <cstdint>

// 交换链图像数量上限，超过则视为异常配置
enum { MAX_SWAPCHAIN_IMAGES = 8 };

struct VulkanContext {
#ifndef __ANDROID__
    GLFWwindow* window;
#else
    ANativeWindow* window;
#endif

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    bool debugUtilsEnabled;  // VK_EXT_debug_utils 是否可用，决定命令标记能不能打
    VkSurfaceKHR surface;

    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceProperties physicalDeviceProperties;
    VkPhysicalDeviceMemoryProperties memoryProperties;

    uint32_t queueFamilyIndex;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;

    VkSwapchainKHR swapchain;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    VkPresentModeKHR presentMode;
    uint32_t swapchainImageCount;
    VkImage swapchainImages[MAX_SWAPCHAIN_IMAGES];
    VkImageView swapchainImageViews[MAX_SWAPCHAIN_IMAGES];
};

// 建实例。表面由 createWindowSurface 单独建，因为安卓上要等系统把 ANativeWindow
// 送过来才能建；物理设备的挑选要看表面是否支持呈现，所以顺序固定为：
// createVulkanContext -> createWindowSurface -> createGraphicsDevice -> createSwapchain
void createVulkanContext(VulkanContext& ctx, bool enableValidation);
void destroyVulkanContext(VulkanContext& ctx);

// 按平台从窗口句柄建呈现表面
#ifndef __ANDROID__
void createWindowSurface(VulkanContext& ctx, GLFWwindow* window);
#else
void createWindowSurface(VulkanContext& ctx, ANativeWindow* window);
#endif
void destroyWindowSurface(VulkanContext& ctx);

// 选物理设备、建逻辑设备与命令池。依赖表面已经存在，安卓上窗口与表面重建时不需要重做
void createGraphicsDevice(VulkanContext& ctx);

void createSwapchain(VulkanContext& ctx);
void destroySwapchain(VulkanContext& ctx);

// 窗口尺寸变化后重建交换链：等设备上没有未完成的工作，再销毁旧的交换链与图像视图。
// 调用之前必须保证没有任何一帧还在使用旧的交换链图像
void recreateSwapchain(VulkanContext& ctx);

uint32_t findMemoryType(const VulkanContext& ctx, uint32_t typeBits, VkMemoryPropertyFlags properties);
