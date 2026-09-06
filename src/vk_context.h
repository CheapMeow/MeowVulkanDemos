#pragma once

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <cstdint>

// 交换链图像数量上限，超过则视为异常配置
enum { MAX_SWAPCHAIN_IMAGES = 8 };

struct VulkanContext {
    GLFWwindow* window;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
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

void createVulkanContext(VulkanContext& ctx, GLFWwindow* window, bool enableValidation);
void destroyVulkanContext(VulkanContext& ctx);

void createSwapchain(VulkanContext& ctx);
void destroySwapchain(VulkanContext& ctx);

// 窗口尺寸变化后重建交换链：等设备上没有未完成的工作，再销毁旧的交换链与图像视图。
// 调用之前必须保证没有任何一帧还在使用旧的交换链图像
void recreateSwapchain(VulkanContext& ctx);

uint32_t findMemoryType(const VulkanContext& ctx, uint32_t typeBits, VkMemoryPropertyFlags properties);
