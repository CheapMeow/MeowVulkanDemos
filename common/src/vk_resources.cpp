#include "vk_resources.h"

#include "asset_file.h"
#include "vk_check.h"

#include <stb_image.h>

#include <cstring>
#include <vector>

VkCommandBuffer beginOneTimeCommands(const VulkanContext& ctx)
{
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ctx.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    return commandBuffer;
}

void endOneTimeCommands(const VulkanContext& ctx, VkCommandBuffer commandBuffer)
{
    VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submitInfo, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &commandBuffer);
}

void createBuffer(const VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memoryProperties, GpuBuffer& outBuffer)
{
    outBuffer = GpuBuffer();
    outBuffer.size = size;

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device, &bufferInfo, nullptr, &outBuffer.buffer));

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx.device, outBuffer.buffer, &requirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(ctx, requirements.memoryTypeBits, memoryProperties);
    VK_CHECK(vkAllocateMemory(ctx.device, &allocInfo, nullptr, &outBuffer.memory));
    VK_CHECK(vkBindBufferMemory(ctx.device, outBuffer.buffer, outBuffer.memory, 0));

    if ((memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        VK_CHECK(vkMapMemory(ctx.device, outBuffer.memory, 0, size, 0, &outBuffer.mapped));
    }
}

void destroyBuffer(const VulkanContext& ctx, GpuBuffer& buffer)
{
    if (buffer.mapped != nullptr) {
        vkUnmapMemory(ctx.device, buffer.memory);
        buffer.mapped = nullptr;
    }
    vkDestroyBuffer(ctx.device, buffer.buffer, nullptr);
    vkFreeMemory(ctx.device, buffer.memory, nullptr);
    buffer.buffer = VK_NULL_HANDLE;
    buffer.memory = VK_NULL_HANDLE;
}

void uploadBufferData(const VulkanContext& ctx, const GpuBuffer& target, const void* data, VkDeviceSize size)
{
    GpuBuffer staging;
    createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);
    VkBufferCopy copyRegion = {};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, staging.buffer, target.buffer, 1, &copyRegion);
    endOneTimeCommands(ctx, commandBuffer);

    GpuBuffer stagingToRelease = staging;
    destroyBuffer(ctx, stagingToRelease);
}

static void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspect,
                                  uint32_t baseMipLevel, uint32_t mipLevelCount, VkImageLayout oldLayout,
                                  VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = mipLevelCount;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void createTextureFromMemory(const VulkanContext& ctx, const std::vector<unsigned char>& fileBytes, bool srgb,
                             GpuTexture& outTexture)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                                           &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        FATAL("failed to load texture from memory: %s", stbi_failure_reason());
    }

    outTexture = GpuTexture();
    outTexture.width = static_cast<uint32_t>(width);
    outTexture.height = static_cast<uint32_t>(height);

    uint32_t mipLevels = 1;
    uint32_t largestSide = outTexture.width > outTexture.height ? outTexture.width : outTexture.height;
    while (largestSide > 1) {
        largestSide /= 2;
        ++mipLevels;
    }
    outTexture.mipLevels = mipLevels;

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;

    GpuBuffer staging;
    createBuffer(ctx, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging);
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(imageSize));
    stbi_image_free(pixels);

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = outTexture.width;
    imageInfo.extent.height = outTexture.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &imageInfo, nullptr, &outTexture.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(ctx.device, outTexture.image, &requirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &allocInfo, nullptr, &outTexture.memory));
    VK_CHECK(vkBindImageMemory(ctx.device, outTexture.image, outTexture.memory, 0));

    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);

    transitionImageLayout(commandBuffer, outTexture.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent.width = outTexture.width;
    copyRegion.imageExtent.height = outTexture.height;
    copyRegion.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, outTexture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // 逐级缩小生成多级渐远纹理
    int32_t mipWidth = static_cast<int32_t>(outTexture.width);
    int32_t mipHeight = static_cast<int32_t>(outTexture.height);
    for (uint32_t level = 1; level < mipLevels; ++level) {
        transitionImageLayout(commandBuffer, outTexture.image, VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = mipWidth;
        blit.srcOffsets[1].y = mipHeight;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1].x = mipWidth > 1 ? mipWidth / 2 : 1;
        blit.dstOffsets[1].y = mipHeight > 1 ? mipHeight / 2 : 1;
        blit.dstOffsets[1].z = 1;

        vkCmdBlitImage(commandBuffer, outTexture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outTexture.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        transitionImageLayout(commandBuffer, outTexture.image, VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
    }

    transitionImageLayout(commandBuffer, outTexture.image, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    endOneTimeCommands(ctx, commandBuffer);

    GpuBuffer stagingToRelease = staging;
    destroyBuffer(ctx, stagingToRelease);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &outTexture.view));

    std::printf("loaded texture: %ux%u, mip levels %u\n", outTexture.width, outTexture.height, mipLevels);
}

void createAttachmentTexture(const VulkanContext& ctx, uint32_t width, uint32_t height, VkFormat format,
                             VkImageUsageFlags usage, VkImageAspectFlags aspect, GpuTexture& outTexture)
{
    outTexture = GpuTexture();
    outTexture.width = width;
    outTexture.height = height;
    outTexture.mipLevels = 1;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &imageInfo, nullptr, &outTexture.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(ctx.device, outTexture.image, &requirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &allocInfo, nullptr, &outTexture.memory));
    VK_CHECK(vkBindImageMemory(ctx.device, outTexture.image, outTexture.memory, 0));

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTexture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &outTexture.view));
}

void destroyTexture(const VulkanContext& ctx, GpuTexture& texture)
{
    vkDestroyImageView(ctx.device, texture.view, nullptr);
    vkDestroyImage(ctx.device, texture.image, nullptr);
    vkFreeMemory(ctx.device, texture.memory, nullptr);
    texture.image = VK_NULL_HANDLE;
    texture.view = VK_NULL_HANDLE;
    texture.memory = VK_NULL_HANDLE;
}

VkSampler createLinearSampler(const VulkanContext& ctx, uint32_t mipLevels)
{
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = ctx.physicalDeviceProperties.limits.maxSamplerAnisotropy;
    samplerInfo.maxLod = static_cast<float>(mipLevels);

    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler));
    return sampler;
}

VkShaderModule loadShaderModuleFromMemory(const VulkanContext& ctx,
                                          const std::vector<unsigned char>& spirvBytes)
{
    if (spirvBytes.empty() || spirvBytes.size() % 4 != 0) {
        FATAL("shader bytecode size %zu is not a whole number of 32-bit words", spirvBytes.size());
    }

    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirvBytes.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(spirvBytes.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx.device, &moduleInfo, nullptr, &shaderModule));
    return shaderModule;
}

void createBackpackMaterialTextures(const VulkanContext& ctx, MaterialTextures& outMaterial)
{
    createTextureFromMemory(ctx, readAssetBytes("backpack/diffuse.jpg"), true, outMaterial.albedo);
    createTextureFromMemory(ctx, readAssetBytes("backpack/normal.png"), false, outMaterial.normal);
    createTextureFromMemory(ctx, readAssetBytes("backpack/specular.jpg"), false, outMaterial.metallic);
    createTextureFromMemory(ctx, readAssetBytes("backpack/roughness.jpg"), false, outMaterial.roughness);
    createTextureFromMemory(ctx, readAssetBytes("backpack/ao.jpg"), false, outMaterial.ambientOcclusion);
    outMaterial.sampler = createLinearSampler(ctx, outMaterial.albedo.mipLevels);
}

void destroyBackpackMaterialTextures(const VulkanContext& ctx, MaterialTextures& material)
{
    vkDestroySampler(ctx.device, material.sampler, nullptr);
    destroyTexture(ctx, material.ambientOcclusion);
    destroyTexture(ctx, material.roughness);
    destroyTexture(ctx, material.metallic);
    destroyTexture(ctx, material.normal);
    destroyTexture(ctx, material.albedo);
}

VkDescriptorSet allocateDescriptorSet(const VulkanContext& ctx, VkDescriptorPool pool,
                                      VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device, &allocInfo, &set));
    return set;
}

void writeBufferDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding,
                           VkDescriptorType type, const GpuBuffer& buffer)
{
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = buffer.size;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}

void writeImageDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding, VkImageView view,
                          VkSampler sampler)
{
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}
