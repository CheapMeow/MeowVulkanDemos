#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat HZB_FORMAT = VK_FORMAT_R32_SFLOAT;

const char* hzbExtremeName(uint32_t extreme)
{
    return extreme == 0 ? "farthest" : "nearest";
}

const char* hzbLevelModeName(uint32_t levelMode)
{
    return levelMode == 0 ? "fixed" : "auto";
}

const char* hzbDepthSourceName(bool currentFrameDepth)
{
    return currentFrameDepth ? "current" : "previous";
}

static float hash01(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0xffffffu) / static_cast<float>(0x1000000u);
}

// 场景：三块大的遮挡物挡在前面，后面散布大量小方块。遮挡物只挡住下半部分，
// 下缘被挡住的方块会被剔除，跨在遮挡物边缘上的方块不该被剔除
static void buildInstances(std::vector<glm::vec4>& instances, uint32_t& outCount)
{
    instances.clear();
    const uint32_t occluderCount = 3;
    for (uint32_t i = 0; i < occluderCount; ++i) {
        const float x = -1.1f + static_cast<float>(i) * 1.1f;
        instances.push_back(glm::vec4(x, -0.30f, -1.2f, 1.1f));
        instances.push_back(glm::vec4(0.25f, 0.30f, 0.75f, 1.0f));
    }

    const uint32_t scattered = 20000;
    for (uint32_t i = 0; i < scattered; ++i) {
        const float x = -2.4f + 4.8f * hash01(i * 13u + 1u);
        const float y = -1.6f + 3.2f * hash01(i * 17u + 5u);
        const float z = -2.4f - 2.6f * hash01(i * 19u + 7u);
        const float size = 0.06f + 0.10f * hash01(i * 23u + 11u);
        const float hue = hash01(i * 29u + 13u);
        // 三个通道都保持在正数范围，着色里的幂运算才有定义
        const glm::vec3 color =
            glm::vec3(0.45f + 0.55f * sin(hue * 6.2831853f), 0.45f + 0.55f * sin(hue * 6.2831853f + 2.09f),
                      0.45f + 0.55f * sin(hue * 6.2831853f + 4.18f));
        instances.push_back(glm::vec4(x, y, z, size));
        instances.push_back(glm::vec4(color, 1.0f));
    }
    outCount = occluderCount + scattered;
}

static void createDepthRenderPass(const VulkanContext& ctx, HzbRenderer& renderer)
{
    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = DEPTH_FORMAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 深度要留给后面构建金字塔的通道采样
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference depthRef = {};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.depthRenderPass));
}

// 两个场景通道只有深度的负载操作不同：上一帧深度时清空，当前帧深度时沿用在预通道里写好的深度
static VkRenderPass createSceneRenderPass(const VulkanContext& ctx, bool loadDepth)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = ctx.swapchainFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = loadDepth ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef = {};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderPass));
    return renderPass;
}

static void createOutputRenderPass(const VulkanContext& ctx, HzbRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = ctx.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.outputRenderPass));
}

// 金字塔的图像先转成通用布局并填成最远深度，第一帧的剔除不会误剔任何实例
static void initializeHzbLevels(const VulkanContext& ctx, HzbRenderer& renderer)
{
    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);

    VkImageMemoryBarrier barriers[HZB_LEVEL_COUNT] = {};
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        barriers[level].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[level].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[level].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[level].srcAccessMask = 0;
        barriers[level].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[level].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[level].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[level].image = renderer.hzbLevels[level].image;
        barriers[level].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[level].subresourceRange.levelCount = 1;
        barriers[level].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, HZB_LEVEL_COUNT, barriers);

    VkClearColorValue clearValue = {};
    clearValue.float32[0] = 1.0f;
    clearValue.float32[1] = 1.0f;
    clearValue.float32[2] = 1.0f;
    clearValue.float32[3] = 1.0f;
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(commandBuffer, renderer.hzbLevels[level].image, VK_IMAGE_LAYOUT_GENERAL,
                             &clearValue, 1, &range);

        barriers[level].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[level].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, HZB_LEVEL_COUNT, barriers);

    endOneTimeCommands(ctx, commandBuffer);
}

static void createSwapchainTargets(const VulkanContext& ctx, HzbRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height,
                            ctx.swapchainFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.colorTexture);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.depthTexture);

    VkImageView depthView[1] = { renderer.depthTexture.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.depthRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = depthView;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.depthFramebuffer));

    VkImageView sceneViews[2] = { renderer.colorTexture.view, renderer.depthTexture.view };
    framebufferInfo.renderPass = renderer.sceneClearRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = sceneViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sceneClearFramebuffer));

    framebufferInfo.renderPass = renderer.sceneLoadRenderPass;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sceneLoadFramebuffer));

    // 金字塔两层：第一层是全分辨率的八分之一，第二层是第一层的八分之一
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        const uint32_t divisor = 8u << (level * 3u);
        createAttachmentTexture(ctx, std::max(1u, ctx.swapchainExtent.width / divisor),
                                std::max(1u, ctx.swapchainExtent.height / divisor), HZB_FORMAT,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, renderer.hzbLevels[level]);
    }
    initializeHzbLevels(ctx, renderer);

    renderer.outputFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView views[1] = { ctx.swapchainImageViews[i] };
        framebufferInfo.renderPass = renderer.outputRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = views;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.outputFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, HzbRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        destroyTexture(ctx, renderer.hzbLevels[level]);
    }
    vkDestroyFramebuffer(ctx.device, renderer.sceneLoadFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.sceneClearFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.depthFramebuffer, nullptr);
    renderer.sceneLoadFramebuffer = VK_NULL_HANDLE;
    renderer.sceneClearFramebuffer = VK_NULL_HANDLE;
    renderer.depthFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.depthTexture);
    destroyTexture(ctx, renderer.colorTexture);
}

static void createDescriptorLayouts(const VulkanContext& ctx, HzbRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    for (uint32_t i = 2; i <= 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.textureSetLayout));

    VkDescriptorSetLayoutBinding resourceBindings[3] = {};
    resourceBindings[0].binding = 0;
    resourceBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resourceBindings[0].descriptorCount = 1;
    resourceBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resourceBindings[1].binding = 1;
    resourceBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resourceBindings[1].descriptorCount = 1;
    resourceBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resourceBindings[2].binding = 2;
    resourceBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resourceBindings[2].descriptorCount = 1;
    resourceBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo resourceLayoutInfo = {};
    resourceLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    resourceLayoutInfo.bindingCount = 3;
    resourceLayoutInfo.pBindings = resourceBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &resourceLayoutInfo, nullptr,
                                         &renderer.resourceSetLayout));

    VkDescriptorSetLayoutBinding hzbBinding = {};
    hzbBinding.binding = 0;
    hzbBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    hzbBinding.descriptorCount = 1;
    hzbBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo hzbLayoutInfo = {};
    hzbLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    hzbLayoutInfo.bindingCount = 1;
    hzbLayoutInfo.pBindings = &hzbBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &hzbLayoutInfo, nullptr, &renderer.hzbSetLayout));

    VkDescriptorPoolSize poolSizes[4] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 32;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 32;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[3].descriptorCount = 8;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeImageDescriptors(const VulkanContext& ctx, HzbRenderer& renderer)
{
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageView = renderer.hzbLevels[level].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = renderer.hzbSets[level];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    }
}

static void writeDescriptors(const VulkanContext& ctx, HzbRenderer& renderer, uint32_t index)
{
    HzbFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.visibleBuffer);

    writeBufferDescriptor(ctx, frame.cullSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeBufferDescriptor(ctx, frame.cullSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    // 剔除通道按层级选择公式读两级金字塔，两级都绑上
    writeImageDescriptor(ctx, frame.cullSet, 2, renderer.hzbLevels[0].view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);
    writeImageDescriptor(ctx, frame.cullSet, 3, renderer.hzbLevels[1].view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);

    writeBufferDescriptor(ctx, frame.presentSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);
    // 金字塔是单通道浮点格式，用最近点采样，两个层级的格子边界也更清楚
    writeImageDescriptor(ctx, frame.presentSet, 2, renderer.hzbLevels[0].view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);
    writeImageDescriptor(ctx, frame.presentSet, 3, renderer.colorTexture.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.presentSet, 4, renderer.hzbLevels[1].view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);

    // 金字塔构建的源：第一层读全分辨率深度，第二层读第一层
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        VkDescriptorSet set = frame.pyramidSets[level];
        writeBufferDescriptor(ctx, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
        writeBufferDescriptor(ctx, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.instanceBuffer);
        if (level == 0) {
            // 深度附件在通道结束时就是通用布局
            writeImageDescriptor(ctx, set, 2, renderer.depthTexture.view, renderer.nearestSampler,
                                 VK_IMAGE_LAYOUT_GENERAL);
        } else {
            writeImageDescriptor(ctx, set, 2, renderer.hzbLevels[level - 1].view, renderer.nearestSampler,
                                 VK_IMAGE_LAYOUT_GENERAL);
        }
    }

    writeBufferDescriptor(ctx, frame.cullResourceSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.visibleFlagBuffer);
    writeBufferDescriptor(ctx, frame.cullResourceSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.visibleBuffer);
    writeBufferDescriptor(ctx, frame.cullResourceSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.indirectBuffer);
}

static VkPipelineShaderStageCreateInfo makeShaderStage(VkShaderStageFlagBits stage, VkShaderModule module)
{
    VkPipelineShaderStageCreateInfo stageInfo = {};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = stage;
    stageInfo.module = module;
    stageInfo.pName = "main";
    return stageInfo;
}

static VkPipeline createGraphicsPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                         VkRenderPass renderPass, const char* vertexShader,
                                         const char* fragmentShader, bool hasColorAttachment,
                                         bool depthTestEnabled, VkCullModeFlags cullMode,
                                         bool directInstance)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(vertexShader));
    VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(fragmentShader));

    // 顶点着色器用特化常量区分两种实例编号来源
    const VkBool32 directInstanceValue = directInstance ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry specializationEntry = {};
    specializationEntry.constantID = 0;
    specializationEntry.offset = 0;
    specializationEntry.size = sizeof(VkBool32);
    VkSpecializationInfo specializationInfo = {};
    specializationInfo.mapEntryCount = 1;
    specializationInfo.pMapEntries = &specializationEntry;
    specializationInfo.dataSize = sizeof(VkBool32);
    specializationInfo.pData = &directInstanceValue;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);
    stages[0].pSpecializationInfo = &specializationInfo;

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = cullMode;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = depthTestEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthTestEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = hasColorAttachment ? 1u : 0u;
    colorBlend.pAttachments = hasColorAttachment ? &blendAttachment : nullptr;

    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    return pipeline;
}

static VkPipeline createComputePipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                        const char* shader)
{
    VkShaderModule module = loadShaderModuleFromMemory(ctx, readAssetBytes(shader));
    VkPipelineShaderStageCreateInfo stage = makeShaderStage(VK_SHADER_STAGE_COMPUTE_BIT, module);
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    vkDestroyShaderModule(ctx.device, module, nullptr);
    return pipeline;
}

void createRenderer(const VulkanContext& ctx, HzbRenderer& renderer)
{
    renderer = HzbRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<glm::vec4> instances;
    buildInstances(instances, renderer.instanceCount);
    // 有序压缩用一个线程组处理 1024 乘 20 个元素
    if (renderer.instanceCount > SCAN_CAPACITY) {
        FATAL("instance count %u exceeds the compaction capacity %d", renderer.instanceCount,
              static_cast<int>(SCAN_CAPACITY));
    }
    createBuffer(ctx, sizeof(glm::vec4) * instances.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.instanceBuffer);
    std::memcpy(renderer.instanceBuffer.mapped, instances.data(), sizeof(glm::vec4) * instances.size());

    std::vector<uint16_t> indices;
    for (uint16_t face = 0; face < 6; ++face) {
        const uint16_t base = static_cast<uint16_t>(face * 4);
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
    createBuffer(ctx, sizeof(uint16_t) * indices.size(),
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.indexBuffer);
    uploadBufferData(ctx, renderer.indexBuffer, indices.data(), sizeof(uint16_t) * indices.size());

    createBuffer(ctx, sizeof(uint32_t) * renderer.instanceCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.visibleBuffer);
    createBuffer(ctx, sizeof(uint32_t) * renderer.instanceCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.visibleFlagBuffer);
    createBuffer(ctx, sizeof(VkDrawIndexedIndirectCommand),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.indirectBuffer);
    // 内容由压缩通道每帧写出，这里先把第一帧之前的回读值清成零
    VkDrawIndexedIndirectCommand initialCommand = {};
    initialCommand.indexCount = 36;
    std::memcpy(renderer.indirectBuffer.mapped, &initialCommand, sizeof(initialCommand));

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.linearSampler));
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.nearestSampler));

    createDepthRenderPass(ctx, renderer);
    renderer.sceneClearRenderPass = createSceneRenderPass(ctx, false);
    renderer.sceneLoadRenderPass = createSceneRenderPass(ctx, true);
    createOutputRenderPass(ctx, renderer);
    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.textureSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.presentPipelineLayout));

    const VkDescriptorSetLayout cullLayouts[2] = { renderer.textureSetLayout, renderer.resourceSetLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = cullLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.cullPipelineLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::vec4);
    const VkDescriptorSetLayout hzbLayouts[2] = { renderer.textureSetLayout, renderer.hzbSetLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = hzbLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.hzbPipelineLayout));

    renderer.boxPipeline =
        createGraphicsPipeline(ctx, renderer.scenePipelineLayout, renderer.sceneClearRenderPass,
                               DEMO_SHADER_DIR "/box.vert.spv", DEMO_SHADER_DIR "/box.frag.spv", true,
                               true, VK_CULL_MODE_BACK_BIT, false);
    renderer.depthPipeline =
        createGraphicsPipeline(ctx, renderer.scenePipelineLayout, renderer.depthRenderPass,
                               DEMO_SHADER_DIR "/box.vert.spv", DEMO_SHADER_DIR "/depth_only.frag.spv",
                               false, true, VK_CULL_MODE_BACK_BIT, true);
    renderer.presentPipeline =
        createGraphicsPipeline(ctx, renderer.presentPipelineLayout, renderer.outputRenderPass,
                               DEMO_SHADER_DIR "/fullscreen.vert.spv", DEMO_SHADER_DIR "/present.frag.spv",
                               true, false, VK_CULL_MODE_NONE, false);

    renderer.cullPipeline = createComputePipeline(ctx, renderer.cullPipelineLayout,
                                                  DEMO_SHADER_DIR "/cull.comp.spv");
    renderer.scanPipeline = createComputePipeline(ctx, renderer.cullPipelineLayout,
                                                  DEMO_SHADER_DIR "/scan.comp.spv");
    renderer.hzbPipeline = createComputePipeline(ctx, renderer.hzbPipelineLayout,
                                                 DEMO_SHADER_DIR "/hzb.comp.spv");

    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        renderer.hzbSets[level] = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.hzbSetLayout);
    }
    writeImageDescriptors(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        HzbFrameResources& frame = renderer.frames[i];

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = ctx.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &frame.commandBuffer));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(ctx.device, &fenceInfo, nullptr, &frame.inFlight));

        createBuffer(ctx, sizeof(HzbSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.uniformBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.cullSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.presentSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.cullResourceSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.resourceSetLayout);
        for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
            frame.pyramidSets[level] =
                allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        }
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));

        frame.timestampsValid = false;
        if (renderer.timestampsSupported) {
            VkQueryPoolCreateInfo queryPoolInfo = {};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPoolInfo.queryCount = QUERY_COUNT;
            VK_CHECK(vkCreateQueryPool(ctx.device, &queryPoolInfo, nullptr, &frame.timestampPool));
        }
    }
}

void destroyRenderer(const VulkanContext& ctx, HzbRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        HzbFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.hzbPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scanPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.cullPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.depthPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.boxPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.hzbPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.cullPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.presentPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.hzbSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.resourceSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.textureSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneLoadRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneClearRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.depthRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.indirectBuffer);
    destroyBuffer(ctx, renderer.visibleFlagBuffer);
    destroyBuffer(ctx, renderer.visibleBuffer);
    destroyBuffer(ctx, renderer.indexBuffer);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, HzbRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    writeImageDescriptors(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

// 深度预通道：绘制全部实例，把当前帧的深度写出来
static void recordDepthPrepass(const VulkanContext& ctx, HzbRenderer& renderer,
                               HzbFrameResources& frame)
{
    VkClearValue clearValue = {};
    clearValue.depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderer.depthRenderPass;
    beginInfo.framebuffer = renderer.depthFramebuffer;
    beginInfo.renderArea.extent = ctx.swapchainExtent;
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clearValue;

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    vkCmdBeginRenderPass(frame.commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.depthPipeline);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(frame.commandBuffer, 36, renderer.instanceCount, 0, 0, 0);
    vkCmdEndRenderPass(frame.commandBuffer);
}

// 遮挡剔除：读金字塔把每个实例的可见标志写出来
static void recordCull(HzbRenderer& renderer, HzbFrameResources& frame)
{
    const VkDescriptorSet cullSets[2] = { frame.cullSet, frame.cullResourceSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            renderer.cullPipelineLayout, 0, 2, cullSets, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.cullPipeline);
    vkCmdDispatch(frame.commandBuffer, (renderer.instanceCount + 63) / 64, 1, 1);
}

// 有序压缩：按实例编号顺序写出可见列表与间接绘制命令
static void recordCompaction(HzbRenderer& renderer, HzbFrameResources& frame)
{
    VkBufferMemoryBarrier flagBarrier = {};
    flagBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    flagBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    flagBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    flagBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    flagBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    flagBarrier.buffer = renderer.visibleFlagBuffer.buffer;
    flagBarrier.offset = 0;
    flagBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &flagBarrier, 0, nullptr);

    const VkDescriptorSet cullSets[2] = { frame.cullSet, frame.cullResourceSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            renderer.cullPipelineLayout, 0, 2, cullSets, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.scanPipeline);
    vkCmdDispatch(frame.commandBuffer, 1, 1, 1);

    VkBufferMemoryBarrier barriers[3] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = renderer.indirectBuffer.buffer;
    barriers[0].offset = 0;
    barriers[0].size = VK_WHOLE_SIZE;
    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].buffer = renderer.visibleBuffer.buffer;
    barriers[1].offset = 0;
    barriers[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                         nullptr, 2, barriers, 0, nullptr);
}

// 金字塔：第一层由深度降采样得到，第二层由第一层得到
static void recordPyramidBuild(const VulkanContext& ctx, HzbRenderer& renderer, HzbFrameResources& frame)
{
    VkImageMemoryBarrier depthRead = {};
    depthRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthRead.image = renderer.depthTexture.image;
    depthRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthRead.subresourceRange.levelCount = 1;
    depthRead.subresourceRange.layerCount = 1;

    // 上一帧的金字塔已经被剔除通道读完，这里改成写入
    VkImageMemoryBarrier hzbWrite[HZB_LEVEL_COUNT] = {};
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        hzbWrite[level].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hzbWrite[level].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        hzbWrite[level].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        hzbWrite[level].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        hzbWrite[level].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hzbWrite[level].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzbWrite[level].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzbWrite[level].image = renderer.hzbLevels[level].image;
        hzbWrite[level].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hzbWrite[level].subresourceRange.levelCount = 1;
        hzbWrite[level].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer,
                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depthRead);
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         HZB_LEVEL_COUNT, hzbWrite);

    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        const VkExtent2D sourceExtent =
            level == 0 ? ctx.swapchainExtent
                       : VkExtent2D{ renderer.hzbLevels[level - 1].width, renderer.hzbLevels[level - 1].height };
        const VkDescriptorSet sets[2] = { frame.pyramidSets[level], renderer.hzbSets[level] };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.hzbPipelineLayout, 0, 2, sets, 0, nullptr);
        const float params[4] = { static_cast<float>(sourceExtent.width),
                                  static_cast<float>(sourceExtent.height),
                                  static_cast<float>(renderer.hzbLevels[level].width),
                                  static_cast<float>(renderer.hzbLevels[level].height) };
        vkCmdPushConstants(frame.commandBuffer, renderer.hzbPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(params), params);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.hzbPipeline);
        vkCmdDispatch(frame.commandBuffer, (renderer.hzbLevels[level].width + 7) / 8,
                      (renderer.hzbLevels[level].height + 7) / 8, 1);

        if (level + 1 < HZB_LEVEL_COUNT) {
            // 下一级要采样这一级
            VkImageMemoryBarrier levelRead = hzbWrite[level];
            levelRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            levelRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &levelRead);
        }
    }

    // 金字塔要在本帧被剔除通道与输出通道读取
    VkImageMemoryBarrier hzbRead[HZB_LEVEL_COUNT] = {};
    for (uint32_t level = 0; level < HZB_LEVEL_COUNT; ++level) {
        hzbRead[level] = hzbWrite[level];
        hzbRead[level].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hzbRead[level].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, HZB_LEVEL_COUNT, hzbRead);
}

bool drawFrame(const VulkanContext& ctx, HzbRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const HzbSceneUniform& uniform, FrameStatistics& outStatistics)
{
    HzbFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    // 上一帧的可见数量
    VkDrawIndexedIndirectCommand previousCommand = {};
    std::memcpy(&previousCommand, renderer.indirectBuffer.mapped, sizeof(previousCommand));
    outStatistics.visibleInstanceCount = previousCommand.instanceCount;
    outStatistics.culledInstanceCount = renderer.instanceCount - previousCommand.instanceCount;

    outStatistics.gpuPrepassMilliseconds = 0.0;
    outStatistics.gpuCullMilliseconds = 0.0;
    outStatistics.gpuPyramidMilliseconds = 0.0;
    outStatistics.gpuSceneMilliseconds = 0.0;
    outStatistics.gpuMilliseconds = 0.0;
    if (renderer.timestampsSupported && frame.timestampsValid) {
        uint64_t timestamps[QUERY_COUNT] = {};
        VK_CHECK(vkGetQueryPoolResults(ctx.device, frame.timestampPool, 0, QUERY_COUNT, sizeof(timestamps),
                                       timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        const double period = static_cast<double>(renderer.timestampPeriodNanoseconds) / 1000000.0;
        outStatistics.gpuPrepassMilliseconds =
            static_cast<double>(timestamps[QUERY_PREPASS_END] - timestamps[QUERY_PREPASS_BEGIN]) * period;
        outStatistics.gpuCullMilliseconds =
            static_cast<double>(timestamps[QUERY_CULL_END] - timestamps[QUERY_CULL_BEGIN]) * period;
        outStatistics.gpuPyramidMilliseconds =
            static_cast<double>(timestamps[QUERY_PYRAMID_END] - timestamps[QUERY_PYRAMID_BEGIN]) * period;
        outStatistics.gpuSceneMilliseconds =
            static_cast<double>(timestamps[QUERY_SCENE_END] - timestamps[QUERY_SCENE_BEGIN]) * period;
        outStatistics.gpuMilliseconds = outStatistics.gpuPrepassMilliseconds +
                                       outStatistics.gpuCullMilliseconds +
                                       outStatistics.gpuPyramidMilliseconds +
                                       outStatistics.gpuSceneMilliseconds;
    }

    uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX,
                                                         frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquireResult);
    }
    if (renderer.imageFences[imageIndex] != VK_NULL_HANDLE) {
        VK_CHECK(vkWaitForFences(ctx.device, 1, &renderer.imageFences[imageIndex], VK_TRUE, UINT64_MAX));
    }
    renderer.imageFences[imageIndex] = frame.inFlight;
    VK_CHECK(vkResetFences(ctx.device, 1, &frame.inFlight));

    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(HzbSceneUniform));

    const bool currentFrameDepth = uniform.miscParams.w > 0.5;

    const double recordBeginStart = nowSeconds();
    VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    if (renderer.timestampsSupported) {
        vkCmdResetQueryPool(frame.commandBuffer, frame.timestampPool, 0, QUERY_COUNT);
    }
    outStatistics.cpuRecordBeginMilliseconds = (nowSeconds() - recordBeginStart) * 1000.0;

    uint32_t drawCallCount = 0;

    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_PREPASS_BEGIN);
    }
    const double prepassStart = nowSeconds();
    if (currentFrameDepth) {
        recordDepthPrepass(ctx, renderer, frame);
        ++drawCallCount;
    }
    outStatistics.cpuRecordPrepassMilliseconds = (nowSeconds() - prepassStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_PREPASS_END);
    }

    if (currentFrameDepth) {
        // 当前帧深度：先把金字塔建出来，再用它剔除
        if (renderer.timestampsSupported) {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                                QUERY_PYRAMID_BEGIN);
        }
        const double pyramidStart = nowSeconds();
        recordPyramidBuild(ctx, renderer, frame);
        outStatistics.cpuRecordPyramidMilliseconds = (nowSeconds() - pyramidStart) * 1000.0;
        if (renderer.timestampsSupported) {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                                QUERY_PYRAMID_END);
        }
    }

    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_CULL_BEGIN);
    }
    const double cullStart = nowSeconds();
    recordCull(renderer, frame);
    recordCompaction(renderer, frame);
    outStatistics.cpuRecordCullMilliseconds = (nowSeconds() - cullStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_CULL_END);
    }

    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_SCENE_BEGIN);
    }
    // 场景：只画可见列表里的实例
    const double sceneStart = nowSeconds();
    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    VkClearValue clearValues[2] = {};
    clearValues[0].color.float32[0] = 0.02f;
    clearValues[0].color.float32[1] = 0.025f;
    clearValues[0].color.float32[2] = 0.035f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo sceneBegin = {};
    sceneBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    sceneBegin.renderPass = currentFrameDepth ? renderer.sceneLoadRenderPass : renderer.sceneClearRenderPass;
    sceneBegin.framebuffer =
        currentFrameDepth ? renderer.sceneLoadFramebuffer : renderer.sceneClearFramebuffer;
    sceneBegin.renderArea.extent = ctx.swapchainExtent;
    sceneBegin.clearValueCount = 2;
    sceneBegin.pClearValues = clearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &sceneBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.boxPipeline);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect(frame.commandBuffer, renderer.indirectBuffer.buffer, 0, 1,
                             sizeof(VkDrawIndexedIndirectCommand));
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_SCENE_END);
    }

    if (!currentFrameDepth) {
        // 上一帧深度：本帧的金字塔供下一帧的剔除使用
        if (renderer.timestampsSupported) {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                                QUERY_PYRAMID_BEGIN);
        }
        const double pyramidStart = nowSeconds();
        recordPyramidBuild(ctx, renderer, frame);
        outStatistics.cpuRecordPyramidMilliseconds = (nowSeconds() - pyramidStart) * 1000.0;
        if (renderer.timestampsSupported) {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                                QUERY_PYRAMID_END);
        }
    }

    const double presentStart = nowSeconds();
    VkRenderPassBeginInfo outputBegin = {};
    outputBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    outputBegin.renderPass = renderer.outputRenderPass;
    outputBegin.framebuffer = renderer.outputFramebuffers[imageIndex];
    outputBegin.renderArea.extent = ctx.swapchainExtent;
    outputBegin.clearValueCount = 0;

    vkCmdBeginRenderPass(frame.commandBuffer, &outputBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.presentPipelineLayout, 0, 1, &frame.presentSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.presentPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;
    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordPresentMilliseconds = (nowSeconds() - presentStart) * 1000.0;

    const double captureStart = nowSeconds();
    if (input.captureBuffer != nullptr) {
        VkImageMemoryBarrier toTransferSource = {};
        toTransferSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransferSource.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toTransferSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransferSource.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransferSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransferSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSource.image = ctx.swapchainImages[imageIndex];
        toTransferSource.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransferSource.subresourceRange.levelCount = 1;
        toTransferSource.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransferSource);

        VkBufferImageCopy copyRegion = {};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent.width = ctx.swapchainExtent.width;
        copyRegion.imageExtent.height = ctx.swapchainExtent.height;
        copyRegion.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(frame.commandBuffer, ctx.swapchainImages[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input.captureBuffer->buffer, 1, &copyRegion);

        VkImageMemoryBarrier backToPresent = toTransferSource;
        backToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        backToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        backToPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        backToPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &backToPresent);
    }
    outStatistics.cpuRecordCaptureMilliseconds =
        input.captureBuffer != nullptr ? (nowSeconds() - captureStart) * 1000.0 : 0.0;

    const double submitStart = nowSeconds();
    VK_CHECK(vkEndCommandBuffer(frame.commandBuffer));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderer.presentSemaphores[imageIndex];
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submitInfo, frame.inFlight));
    outStatistics.cpuRecordSubmitMilliseconds = (nowSeconds() - submitStart) * 1000.0;

    if (renderer.timestampsSupported) {
        frame.timestampsValid = true;
    }
    outStatistics.drawCallCount = drawCallCount;

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderer.presentSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx.swapchain;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult = vkQueuePresentKHR(ctx.queue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(presentResult);
    }
    return true;
}
