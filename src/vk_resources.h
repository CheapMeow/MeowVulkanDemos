#pragma once

#include "vk_context.h"

#include <string>

struct GpuBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void* mapped;
};

struct GpuTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
};

VkCommandBuffer beginOneTimeCommands(const VulkanContext& ctx);
void endOneTimeCommands(const VulkanContext& ctx, VkCommandBuffer commandBuffer);

void createBuffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memoryProperties, GpuBuffer& outBuffer);
void destroyBuffer(const VulkanContext& ctx, GpuBuffer& buffer);
// 通过暂存缓冲把数据写入仅设备可见的缓冲
void uploadBufferData(const VulkanContext& ctx, const GpuBuffer& target, const void* data, VkDeviceSize size);

void createTextureFromFile(const VulkanContext& ctx, const std::string& path, bool srgb, GpuTexture& outTexture);
void createAttachmentTexture(const VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat format,
                             VkImageUsageFlags usage, VkImageAspectFlags aspect, GpuTexture& outTexture);
void destroyTexture(const VulkanContext& ctx, GpuTexture& texture);

VkSampler createLinearSampler(const VulkanContext& ctx, uint32_t mipLevels);
VkShaderModule loadShaderModule(const VulkanContext& ctx, const std::string& spirvPath);
