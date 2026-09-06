#include "vk_context.h"

#include "vk_check.h"

#include <cstring>
#include <vector>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/)
{
    std::fprintf(stderr, "[validation] %s\n", callbackData->pMessage);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        std::abort();
    }
    return VK_FALSE;
}

static void createInstance(VulkanContext& ctx, bool enableValidation)
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr) {
        FATAL("cannot obtain the instance extensions required by GLFW");
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
    if (enableValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Indirect Draw Demo";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (enableValidation) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &ctx.instance));

    ctx.debugMessenger = VK_NULL_HANDLE;
    if (enableValidation) {
        PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr) {
            FATAL("entry point vkCreateDebugUtilsMessengerEXT not found");
        }

        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = debugMessengerCallback;
        VK_CHECK(createMessenger(ctx.instance, &messengerInfo, nullptr, &ctx.debugMessenger));
    }
}

static void pickPhysicalDevice(VulkanContext& ctx)
{
    uint32_t deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, nullptr));
    if (deviceCount == 0) {
        FATAL("no physical device with Vulkan support was found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &deviceCount, devices.data()));

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosenFamily = UINT32_MAX;
    bool chosenIsDiscrete = false;

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, families.data());

        for (uint32_t family = 0; family < familyCount; ++family) {
            const bool supportsGraphics = (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool supportsCompute = (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            if (!supportsGraphics || !supportsCompute) {
                continue;
            }

            VkBool32 supportsPresent = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], family, ctx.surface, &supportsPresent));
            if (supportsPresent != VK_TRUE) {
                continue;
            }

            const bool isDiscrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (chosen == VK_NULL_HANDLE || (isDiscrete && !chosenIsDiscrete)) {
                chosen = devices[i];
                chosenFamily = family;
                chosenIsDiscrete = isDiscrete;
            }
            break;
        }
    }

    if (chosen == VK_NULL_HANDLE) {
        FATAL("no physical device supports graphics, compute and presentation at the same time");
    }

    ctx.physicalDevice = chosen;
    ctx.queueFamilyIndex = chosenFamily;
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &ctx.physicalDeviceProperties);
    vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &ctx.memoryProperties);

    std::printf("physical device: %s\n", ctx.physicalDeviceProperties.deviceName);
}

static void createLogicalDevice(VulkanContext& ctx)
{
    const float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = ctx.queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures features = {};
    features.samplerAnisotropy = VK_TRUE;
    features.multiDrawIndirect = VK_TRUE;
    features.drawIndirectFirstInstance = VK_TRUE;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(ctx.physicalDevice, &deviceInfo, nullptr, &ctx.device));
    vkGetDeviceQueue(ctx.device, ctx.queueFamilyIndex, 0, &ctx.queue);

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &ctx.commandPool));
}

void createVulkanContext(VulkanContext& ctx, GLFWwindow* window, bool enableValidation)
{
    ctx = VulkanContext();
    ctx.window = window;

    createInstance(ctx, enableValidation);
    VK_CHECK(glfwCreateWindowSurface(ctx.instance, window, nullptr, &ctx.surface));
    pickPhysicalDevice(ctx);
    createLogicalDevice(ctx);
}

void destroyVulkanContext(VulkanContext& ctx)
{
    vkDestroyCommandPool(ctx.device, ctx.commandPool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroySurfaceKHR(ctx.instance, ctx.surface, nullptr);

    if (ctx.debugMessenger != VK_NULL_HANDLE) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(ctx.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyMessenger(ctx.instance, ctx.debugMessenger, nullptr);
    }
    vkDestroyInstance(ctx.instance, nullptr);
}

void createSwapchain(VulkanContext& ctx)
{
    VkSurfaceCapabilitiesKHR capabilities = {};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, ctx.surface, &capabilities));

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, formats.data()));

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = formats[i];
            break;
        }
    }

    uint32_t presentModeCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &presentModeCount, nullptr));
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &presentModeCount,
                                                       presentModes.data()));

    // 性能对比需要不受垂直同步限制的帧率
    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < presentModeCount; ++i) {
        if (presentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            chosenPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(ctx.window, &width, &height);
        extent.width = static_cast<uint32_t>(width);
        extent.height = static_cast<uint32_t>(height);
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }
    if (imageCount > MAX_SWAPCHAIN_IMAGES) {
        imageCount = MAX_SWAPCHAIN_IMAGES;
    }

    VkSwapchainCreateInfoKHR swapchainInfo = {};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = ctx.surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosenFormat.format;
    swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = chosenPresentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device, &swapchainInfo, nullptr, &ctx.swapchain));

    ctx.swapchainFormat = chosenFormat.format;
    ctx.swapchainExtent = extent;
    ctx.presentMode = chosenPresentMode;

    uint32_t actualImageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &actualImageCount, nullptr));
    if (actualImageCount > MAX_SWAPCHAIN_IMAGES) {
        FATAL("swapchain image count %u exceeds the upper limit", actualImageCount);
    }
    VK_CHECK(vkGetSwapchainImagesKHR(ctx.device, ctx.swapchain, &actualImageCount, ctx.swapchainImages));
    ctx.swapchainImageCount = actualImageCount;

    for (uint32_t i = 0; i < actualImageCount; ++i) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = ctx.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ctx.swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &ctx.swapchainImageViews[i]));
    }

    std::printf("swapchain: %ux%u, image count %u, present mode %d\n", extent.width, extent.height,
                actualImageCount, static_cast<int>(chosenPresentMode));
}

void destroySwapchain(VulkanContext& ctx)
{
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        vkDestroyImageView(ctx.device, ctx.swapchainImageViews[i], nullptr);
    }
    vkDestroySwapchainKHR(ctx.device, ctx.swapchain, nullptr);
    ctx.swapchain = VK_NULL_HANDLE;
    ctx.swapchainImageCount = 0;
}

void recreateSwapchain(VulkanContext& ctx)
{
    VK_CHECK(vkDeviceWaitIdle(ctx.device));
    destroySwapchain(ctx);
    createSwapchain(ctx);
}

uint32_t findMemoryType(const VulkanContext& ctx, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    for (uint32_t i = 0; i < ctx.memoryProperties.memoryTypeCount; ++i) {
        const bool typeAllowed = (typeBits & (1u << i)) != 0;
        const bool propertiesMatch =
            (ctx.memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeAllowed && propertiesMatch) {
            return i;
        }
    }
    FATAL("no memory type satisfies the requested properties");
}
