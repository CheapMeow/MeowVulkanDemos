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

uint32_t findMemoryType(const VulkanContext& ctx, uint32_t typeBits, VkMemoryPropertyFlags properties);
