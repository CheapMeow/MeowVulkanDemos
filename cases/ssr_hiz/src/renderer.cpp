#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat NORMAL_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat PYRAMID_FORMAT = VK_FORMAT_R32_SFLOAT;
static const VkFormat STEP_FORMAT = VK_FORMAT_R16_SFLOAT;

const char* marchModeName(uint32_t mode)
{
    if (mode == 0) {
        return "view_space";
    }
    return mode == 1 ? "screen_pixel" : "hiz";
}

const char* visualizationName(uint32_t visualization)
{
    if (visualization == 1) {
        return "pyramids";
    }
    return visualization == 2 ? "step_count" : "off";
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

// 场景：正面一块大镜面墙，前面立着一批高低不同的柱子。镜面反射的射线从墙面出发
// 穿过柱间空隙，绝大部分路径落在没有几何的地方，层次遍历才有整块跳过的空间
static void buildInstances(std::vector<glm::vec4>& instances, uint32_t& outCount)
{
    instances.clear();

    // 镜面墙：很薄的一块板，正面朝向相机
    instances.push_back(glm::vec4(0.0f, 3.6f, -11.0f, 0.0f));
    instances.push_back(glm::vec4(0.62f, 0.65f, 0.70f, 0.80f));
    instances.push_back(glm::vec4(13.0f, 3.6f, 0.05f, 0.03f));

    // 悬浮的方块：数量不多、彼此留出大块空隙，反射射线才有整块跳过的空间
    const uint32_t boxCount = 14;
    for (uint32_t i = 0; i < boxCount; ++i) {
        const float x = -7.0f + 14.0f * hash01(i * 31u + 3u);
        const float y = 0.7f + 5.0f * hash01(i * 37u + 7u);
        const float z = -10.5f + 7.0f * hash01(i * 41u + 11u);
        const float half = 0.20f + 0.22f * hash01(i * 43u + 13u);
        const float hue = hash01(i * 53u + 19u);
        const glm::vec3 color =
            glm::vec3(0.55f + 0.45f * sin(hue * 6.2831853f), 0.55f + 0.45f * sin(hue * 6.2831853f + 2.09f),
                      0.55f + 0.45f * sin(hue * 6.2831853f + 4.18f));
        const float reflectivity = 0.05f + 0.10f * hash01(i * 59u + 23u);
        const float roughness = 0.30f + 0.35f * hash01(i * 61u + 29u);

        instances.push_back(glm::vec4(x, y, z, 0.0f));
        instances.push_back(glm::vec4(color, reflectivity));
        instances.push_back(glm::vec4(half, half, half, roughness));
    }
    outCount = boxCount + 1;
}

static void createSceneRenderPass(const VulkanContext& ctx, SsrRenderer& renderer)
{
    VkAttachmentDescription attachments[3] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        attachments[i].format = i == 0 ? COLOR_FORMAT : NORMAL_FORMAT;
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    attachments[2].format = DEPTH_FORMAT;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRefs[2] = {};
    colorRefs[0].attachment = 0;
    colorRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorRefs[1].attachment = 1;
    colorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef = {};
    depthRef.attachment = 2;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    // 三个附件随后要被金字塔与反射通道采样
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 3;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.sceneRenderPass));
}

static void createReflectRenderPass(const VulkanContext& ctx, SsrRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = COLOR_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format = STEP_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRefs[2] = {};
    colorRefs[0].attachment = 0;
    colorRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorRefs[1].attachment = 1;
    colorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments = colorRefs;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.reflectRenderPass));
}

static void createOutputRenderPass(const VulkanContext& ctx, SsrRenderer& renderer)
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

// 金字塔与反射纹理一直用通用布局，创建时先转换一次；
// 两张反射纹理还要先清成零，第一帧的时域累积才有合法的上一帧可读
static void initializeStorageLayouts(const VulkanContext& ctx, SsrRenderer& renderer)
{
    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);

    VkImageMemoryBarrier barriers[PYRAMID_LEVEL_COUNT * 2] = {};
    uint32_t barrierCount = 0;
    const GpuTexture* textures[2] = { &renderer.pyramidMin, &renderer.pyramidAvg };
    for (uint32_t t = 0; t < 2; ++t) {
        VkImageMemoryBarrier& barrier = barriers[barrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = textures[t]->image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = PYRAMID_LEVEL_COUNT;
        barrier.subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, barrierCount, barriers);

    VkImageMemoryBarrier reflectionBarriers[2] = {};
    for (uint32_t parity = 0; parity < 2; ++parity) {
        VkImageMemoryBarrier& barrier = reflectionBarriers[parity];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = renderer.reflectionColor[parity].image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, reflectionBarriers);

    VkClearColorValue clearValue = {};
    for (uint32_t parity = 0; parity < 2; ++parity) {
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(commandBuffer, renderer.reflectionColor[parity].image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);
        reflectionBarriers[parity].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        reflectionBarriers[parity].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        reflectionBarriers[parity].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        reflectionBarriers[parity].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, reflectionBarriers);

    endOneTimeCommands(ctx, commandBuffer);
}

static void createSwapchainTargets(const VulkanContext& ctx, SsrRenderer& renderer)
{
    const VkImageUsageFlags attachmentUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.colorTexture);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, NORMAL_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.normalTexture);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.depthTexture);

    // 金字塔是一条多级渐远纹理链，每一级再单独建一张只覆盖该级的视图给存储图像写入用
    const VkImageUsageFlags pyramidUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, PYRAMID_FORMAT,
                            pyramidUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.pyramidMin,
                            VK_SAMPLE_COUNT_1_BIT, PYRAMID_LEVEL_COUNT);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, PYRAMID_FORMAT,
                            pyramidUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.pyramidAvg,
                            VK_SAMPLE_COUNT_1_BIT, PYRAMID_LEVEL_COUNT);
    for (uint32_t level = 0; level < PYRAMID_LEVEL_COUNT; ++level) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = PYRAMID_FORMAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = level;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        viewInfo.image = renderer.pyramidMin.image;
        VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &renderer.pyramidMinMips[level]));
        viewInfo.image = renderer.pyramidAvg.image;
        VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &renderer.pyramidAvgMips[level]));
    }

    for (uint32_t parity = 0; parity < 2; ++parity) {
        createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT,
                                attachmentUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, renderer.reflectionColor[parity]);
        createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, STEP_FORMAT,
                                attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.reflectionStep[parity]);
    }
    initializeStorageLayouts(ctx, renderer);

    VkImageView sceneViews[3] = { renderer.colorTexture.view, renderer.normalTexture.view,
                                  renderer.depthTexture.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.sceneRenderPass;
    framebufferInfo.attachmentCount = 3;
    framebufferInfo.pAttachments = sceneViews;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sceneFramebuffer));

    framebufferInfo.renderPass = renderer.reflectRenderPass;
    framebufferInfo.attachmentCount = 2;
    for (uint32_t parity = 0; parity < 2; ++parity) {
        VkImageView reflectionViews[2] = { renderer.reflectionColor[parity].view,
                                           renderer.reflectionStep[parity].view };
        framebufferInfo.pAttachments = reflectionViews;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr,
                                     &renderer.reflectFramebuffers[parity]));
    }

    renderer.outputFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView view[1] = { ctx.swapchainImageViews[i] };
        framebufferInfo.renderPass = renderer.outputRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = view;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.outputFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, SsrRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();

    for (uint32_t parity = 0; parity < 2; ++parity) {
        vkDestroyFramebuffer(ctx.device, renderer.reflectFramebuffers[parity], nullptr);
        renderer.reflectFramebuffers[parity] = VK_NULL_HANDLE;
        destroyTexture(ctx, renderer.reflectionStep[parity]);
        destroyTexture(ctx, renderer.reflectionColor[parity]);
    }
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.sceneFramebuffer = VK_NULL_HANDLE;

    if (renderer.pyramidMin.image != VK_NULL_HANDLE) {
        for (uint32_t level = 0; level < PYRAMID_LEVEL_COUNT; ++level) {
            vkDestroyImageView(ctx.device, renderer.pyramidMinMips[level], nullptr);
            vkDestroyImageView(ctx.device, renderer.pyramidAvgMips[level], nullptr);
        }
    }
    destroyTexture(ctx, renderer.pyramidAvg);
    destroyTexture(ctx, renderer.pyramidMin);
    destroyTexture(ctx, renderer.depthTexture);
    destroyTexture(ctx, renderer.normalTexture);
    destroyTexture(ctx, renderer.colorTexture);
}

static void createDescriptorLayouts(const VulkanContext& ctx, SsrRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[10] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    for (uint32_t i = 2; i <= 9; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 10;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.frameSetLayout));

    VkDescriptorSetLayoutBinding pyramidBindings[2] = {};
    pyramidBindings[0].binding = 0;
    pyramidBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pyramidBindings[0].descriptorCount = 1;
    pyramidBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pyramidBindings[1].binding = 1;
    pyramidBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pyramidBindings[1].descriptorCount = 1;
    pyramidBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo pyramidLayoutInfo = {};
    pyramidLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pyramidLayoutInfo.bindingCount = 2;
    pyramidLayoutInfo.pBindings = pyramidBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &pyramidLayoutInfo, nullptr,
                                         &renderer.pyramidSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 64;
    VkDescriptorPoolSize storageImageSize = {};
    storageImageSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    storageImageSize.descriptorCount = 32;

    VkDescriptorPoolSize allSizes[4] = { poolSizes[0], poolSizes[1], poolSizes[2], storageImageSize };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 48;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = allSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

// 金字塔的每一级的写入目标，以及读取上一级的来源
static void writePyramidDescriptors(const VulkanContext& ctx, SsrRenderer& renderer)
{
    const GpuTexture* textures[2] = { &renderer.pyramidMin, &renderer.pyramidAvg };
    VkDescriptorSet* sets[2] = { renderer.pyramidMinSets, renderer.pyramidAvgSets };
    for (uint32_t t = 0; t < 2; ++t) {
        for (uint32_t level = 0; level < PYRAMID_LEVEL_COUNT; ++level) {
            // 第一级的来源是几何通道的深度缓冲，之后每一级的来源是自己这条金字塔的上一级
            writeImageDescriptor(ctx, sets[t][level], 0,
                                 level == 0 ? renderer.depthTexture.view : textures[t]->view,
                                 renderer.nearestSampler,
                                 level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_GENERAL);

            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageView =
                t == 0 ? renderer.pyramidMinMips[level] : renderer.pyramidAvgMips[level];
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = sets[t][level];
            write.dstBinding = 1;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
        }
    }
}

static void writeDescriptors(const VulkanContext& ctx, SsrRenderer& renderer, uint32_t index, uint32_t parity)
{
    SsrFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.frameSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeBufferDescriptor(ctx, frame.frameSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.frameSet, 2, renderer.colorTexture.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.frameSet, 3, renderer.depthTexture.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.frameSet, 4, renderer.normalTexture.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.frameSet, 5, renderer.pyramidMin.view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);
    writeImageDescriptor(ctx, frame.frameSet, 6, renderer.pyramidAvg.view, renderer.nearestSampler,
                         VK_IMAGE_LAYOUT_GENERAL);
    writeImageDescriptor(ctx, frame.frameSet, 7, renderer.reflectionColor[1 - parity].view,
                         renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.frameSet, 8, renderer.reflectionColor[parity].view,
                         renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.frameSet, 9, renderer.reflectionStep[parity].view,
                         renderer.nearestSampler);
}

static VkPipelineShaderStageCreateInfo makeShaderStage(VkShaderStageFlagBits stage, VkShaderModule module,
                                                       const VkSpecializationInfo* specialization)
{
    VkPipelineShaderStageCreateInfo stageInfo = {};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = stage;
    stageInfo.module = module;
    stageInfo.pName = "main";
    stageInfo.pSpecializationInfo = specialization;
    return stageInfo;
}

struct GraphicsPipelineDesc {
    VkPipelineLayout layout;
    VkRenderPass renderPass;
    const char* vertexShader;
    const char* fragmentShader;
    uint32_t colorAttachmentCount;
    bool depthAttachment;
    VkPrimitiveTopology topology;
    VkCullModeFlags cullMode;
    int specializationConstant;   // 负值表示不设置特化常量
};

static VkPipeline createGraphicsPipeline(const VulkanContext& ctx, const GraphicsPipelineDesc& desc)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(desc.vertexShader));
    VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(desc.fragmentShader));

    const int specializationValue = desc.specializationConstant;
    VkSpecializationMapEntry specializationEntry = {};
    specializationEntry.constantID = 0;
    specializationEntry.offset = 0;
    specializationEntry.size = sizeof(int);
    VkSpecializationInfo specializationInfo = {};
    specializationInfo.mapEntryCount = 1;
    specializationInfo.pMapEntries = &specializationEntry;
    specializationInfo.dataSize = sizeof(int);
    specializationInfo.pData = &specializationValue;
    const VkSpecializationInfo* specialization =
        desc.specializationConstant >= 0 ? &specializationInfo : nullptr;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule, nullptr);
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, specialization);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = desc.topology;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = desc.cullMode;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthAttachment ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthAttachment ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = desc.colorAttachmentCount;
    colorBlend.pAttachments = desc.colorAttachmentCount > 0 ? blendAttachments : nullptr;

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
    pipelineInfo.layout = desc.layout;
    pipelineInfo.renderPass = desc.renderPass;
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
    VkPipelineShaderStageCreateInfo stage = makeShaderStage(VK_SHADER_STAGE_COMPUTE_BIT, module, nullptr);
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    vkDestroyShaderModule(ctx.device, module, nullptr);
    return pipeline;
}

void createRenderer(const VulkanContext& ctx, SsrRenderer& renderer)
{
    renderer = SsrRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<glm::vec4> instances;
    buildInstances(instances, renderer.instanceCount);
    createBuffer(ctx, sizeof(glm::vec4) * instances.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.instanceBuffer);
    std::memcpy(renderer.instanceBuffer.mapped, instances.data(), sizeof(glm::vec4) * instances.size());

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // 层次遍历要按层级取金字塔的任意一级，上界必须放开
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.nearestSampler));

    createSceneRenderPass(ctx, renderer);
    createReflectRenderPass(ctx, renderer);
    createOutputRenderPass(ctx, renderer);
    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.frameSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.reflectPipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.presentPipelineLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::vec4) * 2;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.pyramidSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.pyramidPipelineLayout));

    GraphicsPipelineDesc desc = {};
    desc.layout = renderer.scenePipelineLayout;
    desc.renderPass = renderer.sceneRenderPass;
    desc.vertexShader = DEMO_SHADER_DIR "/scene.vert.spv";
    desc.fragmentShader = DEMO_SHADER_DIR "/gbuffer.frag.spv";
    desc.colorAttachmentCount = 2;
    desc.depthAttachment = true;
    desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    desc.specializationConstant = -1;
    renderer.boxPipeline = createGraphicsPipeline(ctx, desc);

    desc.layout = renderer.reflectPipelineLayout;
    desc.renderPass = renderer.reflectRenderPass;
    desc.vertexShader = DEMO_SHADER_DIR "/fullscreen.vert.spv";
    desc.fragmentShader = DEMO_SHADER_DIR "/reflect.frag.spv";
    desc.colorAttachmentCount = 2;
    desc.depthAttachment = false;
    desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    desc.cullMode = VK_CULL_MODE_NONE;
    for (int mode = 0; mode < MARCH_MODE_COUNT; ++mode) {
        desc.specializationConstant = mode;
        renderer.reflectPipelines[mode] = createGraphicsPipeline(ctx, desc);
    }

    desc.layout = renderer.presentPipelineLayout;
    desc.renderPass = renderer.outputRenderPass;
    desc.fragmentShader = DEMO_SHADER_DIR "/present.frag.spv";
    desc.colorAttachmentCount = 1;
    desc.specializationConstant = -1;
    renderer.presentPipeline = createGraphicsPipeline(ctx, desc);

    renderer.pyramidPipeline =
        createComputePipeline(ctx, renderer.pyramidPipelineLayout, DEMO_SHADER_DIR "/pyramid.comp.spv");

    for (uint32_t level = 0; level < PYRAMID_LEVEL_COUNT; ++level) {
        renderer.pyramidMinSets[level] =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.pyramidSetLayout);
        renderer.pyramidAvgSets[level] =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.pyramidSetLayout);
    }
    writePyramidDescriptors(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SsrFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(SsrUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.uniformBuffer);

        frame.frameSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.frameSetLayout);
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i), 0);

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

void destroyRenderer(const VulkanContext& ctx, SsrRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SsrFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.pyramidPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    for (int mode = 0; mode < MARCH_MODE_COUNT; ++mode) {
        vkDestroyPipeline(ctx.device, renderer.reflectPipelines[mode], nullptr);
    }
    vkDestroyPipeline(ctx.device, renderer.boxPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.pyramidPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.presentPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.reflectPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.pyramidSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.frameSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.reflectRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, SsrRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    writePyramidDescriptors(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i), 0);
    }
}

// 几何通道：地面与柱子写入颜色、视空间法线与深度
static void recordGeometryPass(const VulkanContext& ctx, SsrRenderer& renderer, SsrFrameResources& frame,
                               uint32_t& outDrawCalls)
{
    VkClearValue clearValues[3] = {};
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].color.float32[3] = 1.0f;
    clearValues[2].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderer.sceneRenderPass;
    beginInfo.framebuffer = renderer.sceneFramebuffer;
    beginInfo.renderArea.extent = ctx.swapchainExtent;
    beginInfo.clearValueCount = 3;
    beginInfo.pClearValues = clearValues;

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
                            renderer.scenePipelineLayout, 0, 1, &frame.frameSet, 0, nullptr);

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.boxPipeline);
    vkCmdDraw(frame.commandBuffer, 36, renderer.instanceCount, 0, 0);
    ++outDrawCalls;

    vkCmdEndRenderPass(frame.commandBuffer);
}

// 金字塔：第一级抄深度，之后每一级取上一级的二乘二小块，最小值与平均值各建一条
static void recordPyramidBuild(const VulkanContext& ctx, SsrRenderer& renderer, SsrFrameResources& frame)
{
    for (uint32_t level = 0; level < PYRAMID_LEVEL_COUNT; ++level) {
        if (level > 0) {
            // 上一级刚写完，这一级要读它
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = renderer.pyramidMin.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = level - 1;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
            barrier.image = renderer.pyramidAvg.image;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
        }

        const uint32_t destWidth = std::max(1u, ctx.swapchainExtent.width >> level);
        const uint32_t destHeight = std::max(1u, ctx.swapchainExtent.height >> level);
        const uint32_t sourceWidth =
            level == 0 ? ctx.swapchainExtent.width : std::max(1u, ctx.swapchainExtent.width >> (level - 1));
        const uint32_t sourceHeight =
            level == 0 ? ctx.swapchainExtent.height : std::max(1u, ctx.swapchainExtent.height >> (level - 1));
        const float reduction = level == 0 ? 1.0f : 2.0f;

        for (uint32_t t = 0; t < 2; ++t) {
            const VkDescriptorSet set =
                t == 0 ? renderer.pyramidMinSets[level] : renderer.pyramidAvgSets[level];
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    renderer.pyramidPipelineLayout, 0, 1, &set, 0, nullptr);
            const float params[8] = { static_cast<float>(sourceWidth),
                                      static_cast<float>(sourceHeight),
                                      static_cast<float>(destWidth),
                                      static_cast<float>(destHeight),
                                      reduction,
                                      t == 0 ? 0.0f : 1.0f,
                                      static_cast<float>(level == 0 ? 0 : level - 1),
                                      0.0f };
            vkCmdPushConstants(frame.commandBuffer, renderer.pyramidPipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pyramidPipeline);
            vkCmdDispatch(frame.commandBuffer, (destWidth + 7) / 8, (destHeight + 7) / 8, 1);
        }
    }

    // 金字塔接下来要给反射通道采样
    VkImageMemoryBarrier barriers[2] = {};
    const GpuTexture* textures[2] = { &renderer.pyramidMin, &renderer.pyramidAvg };
    for (uint32_t t = 0; t < 2; ++t) {
        barriers[t].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[t].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[t].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[t].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[t].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[t].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[t].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[t].image = textures[t]->image;
        barriers[t].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[t].subresourceRange.levelCount = PYRAMID_LEVEL_COUNT;
        barriers[t].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
}

// 反射通道：全屏三角形，每个像素沿反射方向步进
static void recordReflectPass(const VulkanContext& ctx, SsrRenderer& renderer, SsrFrameResources& frame,
                              uint32_t parity, const SsrUniform& uniform, uint32_t& outDrawCalls)
{
    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderer.reflectRenderPass;
    beginInfo.framebuffer = renderer.reflectFramebuffers[parity];
    beginInfo.renderArea.extent = ctx.swapchainExtent;
    beginInfo.clearValueCount = 0;

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    const uint32_t mode = std::min(static_cast<uint32_t>(uniform.modeParams.x + 0.5f),
                                   static_cast<uint32_t>(MARCH_MODE_COUNT - 1));

    vkCmdBeginRenderPass(frame.commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.reflectPipelineLayout, 0, 1, &frame.frameSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      renderer.reflectPipelines[mode]);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++outDrawCalls;
    vkCmdEndRenderPass(frame.commandBuffer);
}

bool drawFrame(const VulkanContext& ctx, SsrRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SsrUniform& uniform, FrameStatistics& outStatistics)
{
    SsrFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    outStatistics.gpuGeometryMilliseconds = 0.0;
    outStatistics.gpuPyramidMilliseconds = 0.0;
    outStatistics.gpuReflectMilliseconds = 0.0;
    outStatistics.gpuPresentMilliseconds = 0.0;
    outStatistics.gpuMilliseconds = 0.0;
    if (renderer.timestampsSupported && frame.timestampsValid) {
        uint64_t timestamps[QUERY_COUNT] = {};
        VK_CHECK(vkGetQueryPoolResults(ctx.device, frame.timestampPool, 0, QUERY_COUNT, sizeof(timestamps),
                                       timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        const double period = static_cast<double>(renderer.timestampPeriodNanoseconds) / 1000000.0;
        outStatistics.gpuGeometryMilliseconds =
            static_cast<double>(timestamps[QUERY_GEOMETRY_END] - timestamps[QUERY_GEOMETRY_BEGIN]) * period;
        outStatistics.gpuPyramidMilliseconds =
            static_cast<double>(timestamps[QUERY_PYRAMID_END] - timestamps[QUERY_PYRAMID_BEGIN]) * period;
        outStatistics.gpuReflectMilliseconds =
            static_cast<double>(timestamps[QUERY_REFLECT_END] - timestamps[QUERY_REFLECT_BEGIN]) * period;
        outStatistics.gpuPresentMilliseconds =
            static_cast<double>(timestamps[QUERY_PRESENT_END] - timestamps[QUERY_PRESENT_BEGIN]) * period;
        outStatistics.gpuMilliseconds = outStatistics.gpuGeometryMilliseconds +
                                        outStatistics.gpuPyramidMilliseconds +
                                        outStatistics.gpuReflectMilliseconds +
                                        outStatistics.gpuPresentMilliseconds;
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

    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(SsrUniform));

    const uint32_t parity = static_cast<uint32_t>(frameCounter & 1u);
    writeDescriptors(ctx, renderer, static_cast<uint32_t>(frameCounter % MAX_FRAMES_IN_FLIGHT), parity);

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
                            QUERY_GEOMETRY_BEGIN);
    }
    const double geometryStart = nowSeconds();
    recordGeometryPass(ctx, renderer, frame, drawCallCount);
    outStatistics.cpuRecordGeometryMilliseconds = (nowSeconds() - geometryStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_GEOMETRY_END);
    }

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

    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_REFLECT_BEGIN);
    }
    const double reflectStart = nowSeconds();
    recordReflectPass(ctx, renderer, frame, parity, uniform, drawCallCount);
    outStatistics.cpuRecordReflectMilliseconds = (nowSeconds() - reflectStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_REFLECT_END);
    }

    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_PRESENT_BEGIN);
    }
    const double presentStart = nowSeconds();
    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

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
                            renderer.presentPipelineLayout, 0, 1, &frame.frameSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.presentPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;
    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordPresentMilliseconds = (nowSeconds() - presentStart) * 1000.0;
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool,
                            QUERY_PRESENT_END);
    }

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
    outStatistics.instanceCount = renderer.instanceCount;

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
