#pragma once

#include "vk_context.h"

#include <string>
#include <vector>

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

// 一套 PBR 材质贴图，各 case 共用
struct MaterialTextures {
    GpuTexture albedo;
    GpuTexture normal;
    GpuTexture metallic;
    GpuTexture roughness;
    GpuTexture ambientOcclusion;
    VkSampler sampler;
};

VkCommandBuffer beginOneTimeCommands(const VulkanContext& ctx);
void endOneTimeCommands(const VulkanContext& ctx, VkCommandBuffer commandBuffer);

void createBuffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memoryProperties, GpuBuffer& outBuffer);
void destroyBuffer(const VulkanContext& ctx, GpuBuffer& buffer);
// 通过暂存缓冲把数据写入仅设备可见的缓冲
void uploadBufferData(const VulkanContext& ctx, const GpuBuffer& target, const void* data, VkDeviceSize size);

void createTextureFromMemory(const VulkanContext& ctx, const std::vector<unsigned char>& fileBytes, bool srgb,
                             GpuTexture& outTexture);
void createAttachmentTexture(const VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat format,
                             VkImageUsageFlags usage, VkImageAspectFlags aspect, GpuTexture& outTexture);
void destroyTexture(const VulkanContext& ctx, GpuTexture& texture);

VkSampler createLinearSampler(const VulkanContext& ctx, uint32_t mipLevels);
VkShaderModule loadShaderModuleFromMemory(const VulkanContext& ctx,
                                          const std::vector<unsigned char>& spirvBytes);

// 载入仓库里那套 backpack 的 PBR 贴图（albedo、法线、金属度、粗糙度、环境光遮蔽）
void createBackpackMaterialTextures(const VulkanContext& ctx, MaterialTextures& outMaterial);
void destroyBackpackMaterialTextures(const VulkanContext& ctx, MaterialTextures& material);

VkDescriptorSet allocateDescriptorSet(const VulkanContext& ctx, VkDescriptorPool pool,
                                      VkDescriptorSetLayout layout);
void writeBufferDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding,
                           VkDescriptorType type, const GpuBuffer& buffer);
void writeImageDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding, VkImageView view,
                          VkSampler sampler);
