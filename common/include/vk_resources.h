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
                             GpuTexture& outTexture, VkImageView* outAlternateView = nullptr);
// 用已经解好的 RGBA8 像素建纹理，同样按逐级缩小生成多级渐远纹理。
// outAlternateView 非空时，图像按可换格式创建，并额外给出一张按相反格式解释的视图：
// srgb 为真时主视图是 sRGB、附加视图是线性，srgb 为假时相反
void createTextureFromRgba(const VulkanContext& ctx, uint32_t width, uint32_t height,
                           const unsigned char* pixels, bool srgb, GpuTexture& outTexture,
                           VkImageView* outAlternateView = nullptr);
void createAttachmentTexture(const VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat format,
                             VkImageUsageFlags usage, VkImageAspectFlags aspect, GpuTexture& outTexture,
                             VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
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
