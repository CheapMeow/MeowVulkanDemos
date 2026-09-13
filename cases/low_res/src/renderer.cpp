#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat SCENE_COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat SCENE_NORMAL_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat SCENE_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat LOW_GEOMETRY_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
static const VkFormat GLOW_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

const char* lowResRatioName(uint32_t ratio)
{
    if (ratio == LOWRES_RATIO_HALF) {
        return "half";
    }
    if (ratio == LOWRES_RATIO_QUARTER) {
        return "quarter";
    }
    return "full";
}

const char* lowResUpsampleName(uint32_t upsample)
{
    return upsample == LOWRES_UPSAMPLE_BILATERAL ? "bilateral" : "bilinear";
}

const char* lowResDepthModeName(uint32_t depthMode)
{
    if (depthMode == LOWRES_DEPTH_SUBPASS) {
        return "subpass";
    }
    if (depthMode == LOWRES_DEPTH_RECONSTRUCT) {
        return "reconstruct";
    }
    return "copy";
}

static uint32_t ratioDivisor(uint32_t ratio)
{
    if (ratio == LOWRES_RATIO_HALF) {
        return 2;
    }
    if (ratio == LOWRES_RATIO_QUARTER) {
        return 4;
    }
    return 1;
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

// 把图像一次性转到只读布局。低分辨率几何在从全分辨率重新取的模式下不会被任何通道写过，
// 但描述符里仍然绑着它，先给一个确定的布局，后面的屏障才能如实声明旧布局
static void initializeImageLayout(const VulkanContext& ctx, VkImage image)
{
    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    endOneTimeCommands(ctx, commandBuffer);
}

// 一排相互交叠、绕竖直轴偏转的板：相邻的板之间有明显的深度台阶与法线台阶
static void buildSceneInstances(std::vector<glm::vec4>& instances, uint32_t& outCount)
{
    instances.clear();
    const uint32_t rows = 6;
    const uint32_t columns = 8;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const uint32_t index = row * columns + column;
            const float x = -1.5f + static_cast<float>(column) * 0.43f + (hash01(index * 3u + 1u) - 0.5f) * 0.08f;
            const float y = -0.42f + static_cast<float>(row) * 0.19f;
            const float z = -1.7f - static_cast<float>(row) * 0.72f;
            const float yaw = (column % 2 == 0 ? 1.0f : -1.0f) * (0.55f + 0.07f * static_cast<float>(row));
            const float shade = 0.55f + 0.45f * hash01(index * 7u + 3u);
            const float hue = static_cast<float>(row) / static_cast<float>(rows) * 0.6f +
                              static_cast<float>(column) / static_cast<float>(columns) * 0.3f;
            instances.push_back(glm::vec4(x, y, 0.20f, 0.42f));
            instances.push_back(glm::vec4(yaw, shade, z, hue));
        }
    }
    outCount = rows * columns;
}

// 建一个只有颜色附件的渲染通道，附件数量与格式由调用方给出
static VkRenderPass createColorRenderPass(const VulkanContext& ctx, const VkFormat* formats, uint32_t count,
                                          bool clear, bool toPresent)
{
    VkAttachmentDescription attachments[2] = {};
    VkAttachmentReference references[2] = {};
    for (uint32_t i = 0; i < count; ++i) {
        attachments[i].format = formats[i];
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout =
            toPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        references[i].attachment = i;
        references[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = count;
    subpass.pColorAttachments = references;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = count;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderPass));
    return renderPass;
}

// 场景通道：颜色、法线与深度三张附件
static void createSceneRenderPass(const VulkanContext& ctx, LowResRenderer& renderer)
{
    VkAttachmentDescription attachments[3] = {};
    attachments[0].format = SCENE_COLOR_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[1].format = SCENE_NORMAL_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[2].format = SCENE_DEPTH_FORMAT;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
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

static void createSwapchainTargets(const VulkanContext& ctx, LowResRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_COLOR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneColor);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_NORMAL_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneNormal);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.sceneDepth);

    VkImageView sceneViews[3] = { renderer.sceneColor.view, renderer.sceneNormal.view,
                                  renderer.sceneDepth.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.sceneRenderPass;
    framebufferInfo.attachmentCount = 3;
    framebufferInfo.pAttachments = sceneViews;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sceneFramebuffer));

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

// 低分辨率附件随比例变化，单独建一套
static void createLowResTargets(const VulkanContext& ctx, LowResRenderer& renderer, uint32_t divisor)
{
    const uint32_t width = std::max(1u, ctx.swapchainExtent.width / divisor);
    const uint32_t height = std::max(1u, ctx.swapchainExtent.height / divisor);
    renderer.lowResWidth = width;
    renderer.lowResHeight = height;

    createAttachmentTexture(ctx, width, height, LOW_GEOMETRY_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.lowGeometry);
    createAttachmentTexture(ctx, width, height, GLOW_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.glowColor);

    VkImageView geometryViews[1] = { renderer.lowGeometry.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.downgradeRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = geometryViews;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.downgradeFramebuffer));

    VkImageView glowViews[1] = { renderer.glowColor.view };
    framebufferInfo.renderPass = renderer.glowRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = glowViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.glowFramebuffer));

    VkImageView bothViews[2] = { renderer.glowColor.view, renderer.lowGeometry.view };
    framebufferInfo.renderPass = renderer.glowGeometryRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = bothViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.glowGeometryFramebuffer));

    initializeImageLayout(ctx, renderer.lowGeometry.image);
}

static void destroyLowResTargets(const VulkanContext& ctx, LowResRenderer& renderer)
{
    if (renderer.glowGeometryFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx.device, renderer.glowGeometryFramebuffer, nullptr);
        renderer.glowGeometryFramebuffer = VK_NULL_HANDLE;
    }
    if (renderer.glowFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx.device, renderer.glowFramebuffer, nullptr);
        renderer.glowFramebuffer = VK_NULL_HANDLE;
    }
    if (renderer.downgradeFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(ctx.device, renderer.downgradeFramebuffer, nullptr);
        renderer.downgradeFramebuffer = VK_NULL_HANDLE;
    }
    destroyTexture(ctx, renderer.glowColor);
    destroyTexture(ctx, renderer.lowGeometry);
}

static void destroySwapchainTargets(const VulkanContext& ctx, LowResRenderer& renderer)
{
    destroyLowResTargets(ctx, renderer);
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.sceneFramebuffer = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.sceneDepth);
    destroyTexture(ctx, renderer.sceneNormal);
    destroyTexture(ctx, renderer.sceneColor);
}

static void createDescriptorLayout(const VulkanContext& ctx, LowResRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[7] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    for (uint32_t i = 2; i <= 6; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 32;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeDescriptors(const VulkanContext& ctx, LowResRenderer& renderer, uint32_t index)
{
    LowResFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.sceneSet, 2, renderer.sceneDepth.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 3, renderer.sceneNormal.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 4, renderer.sceneColor.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 5, renderer.lowGeometry.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 6, renderer.glowColor.view, renderer.linearSampler);
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

static void fillFullscreenStates(VkPipelineVertexInputStateCreateInfo& vertexInput,
                                 VkPipelineInputAssemblyStateCreateInfo& inputAssembly,
                                 VkPipelineViewportStateCreateInfo& viewportState,
                                 VkPipelineRasterizationStateCreateInfo& rasterization,
                                 VkPipelineMultisampleStateCreateInfo& multisample,
                                 VkPipelineDepthStencilStateCreateInfo& depthStencil,
                                 VkPipelineDynamicStateCreateInfo& dynamicState,
                                 VkPipelineColorBlendStateCreateInfo& colorBlend,
                                 VkPipelineColorBlendAttachmentState* blendAttachments, uint32_t count)
{
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    static VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    for (uint32_t i = 0; i < count; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = count;
    colorBlend.pAttachments = blendAttachments;
}

static VkPipeline createFullscreenPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                           VkRenderPass renderPass, const char* fragmentShader,
                                           uint32_t attachmentCount)
{
    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
    VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(fragmentShader));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    VkPipelineViewportStateCreateInfo viewportState = {};
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    VkPipelineMultisampleStateCreateInfo multisample = {};
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    fillFullscreenStates(vertexInput, inputAssembly, viewportState, rasterization, multisample, depthStencil,
                         dynamicState, colorBlend, blendAttachments, attachmentCount);

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

void createRenderer(const VulkanContext& ctx, LowResRenderer& renderer)
{
    renderer = LowResRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<glm::vec4> instances;
    buildSceneInstances(instances, renderer.instanceCount);
    createBuffer(ctx, sizeof(glm::vec4) * instances.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.instanceBuffer);
    std::memcpy(renderer.instanceBuffer.mapped, instances.data(), sizeof(glm::vec4) * instances.size());

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

    createSceneRenderPass(ctx, renderer);
    const VkFormat geometryFormats[1] = { LOW_GEOMETRY_FORMAT };
    renderer.downgradeRenderPass = createColorRenderPass(ctx, geometryFormats, 1, false, false);
    const VkFormat glowFormats[1] = { GLOW_FORMAT };
    renderer.glowRenderPass = createColorRenderPass(ctx, glowFormats, 1, false, false);
    const VkFormat glowGeometryFormats[2] = { GLOW_FORMAT, LOW_GEOMETRY_FORMAT };
    renderer.glowGeometryRenderPass = createColorRenderPass(ctx, glowGeometryFormats, 2, false, false);
    const VkFormat outputFormats[1] = { ctx.swapchainFormat };
    renderer.outputRenderPass = createColorRenderPass(ctx, outputFormats, 1, false, true);

    createDescriptorLayout(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createLowResTargets(ctx, renderer, 1);

    // 场景管线
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));

    {
        VkShaderModule vertexModule =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.vert.spv"));
        VkShaderModule fragmentModule =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.frag.spv"));

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
        stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 2;
        colorBlend.pAttachments = blendAttachments;

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
        pipelineInfo.layout = renderer.scenePipelineLayout;
        pipelineInfo.renderPass = renderer.sceneRenderPass;
        pipelineInfo.subpass = 0;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           &renderer.scenePipeline));

        vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
        vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    }

    renderer.downgradePipeline =
        createFullscreenPipeline(ctx, renderer.scenePipelineLayout, renderer.downgradeRenderPass,
                                 DEMO_SHADER_DIR "/downgrade.frag.spv", 1);
    renderer.glowPipeline = createFullscreenPipeline(ctx, renderer.scenePipelineLayout,
                                                     renderer.glowRenderPass, DEMO_SHADER_DIR "/glow.frag.spv",
                                                     1);
    renderer.glowGeometryPipeline =
        createFullscreenPipeline(ctx, renderer.scenePipelineLayout, renderer.glowGeometryRenderPass,
                                 DEMO_SHADER_DIR "/glow_geometry.frag.spv", 2);

    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.compositePipelineLayout));
    renderer.compositePipeline = createFullscreenPipeline(ctx, renderer.compositePipelineLayout,
                                                          renderer.outputRenderPass,
                                                          DEMO_SHADER_DIR "/composite.frag.spv", 1);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        LowResFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(LowResSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));

        frame.timestampsValid = false;
        if (renderer.timestampsSupported) {
            VkQueryPoolCreateInfo queryPoolInfo = {};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPoolInfo.queryCount = 2;
            VK_CHECK(vkCreateQueryPool(ctx.device, &queryPoolInfo, nullptr, &frame.timestampPool));
        }
    }
}

void destroyRenderer(const VulkanContext& ctx, LowResRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        LowResFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.compositePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.compositePipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.glowGeometryPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.glowPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.downgradePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scenePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.glowGeometryRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.glowRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.downgradeRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, LowResRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createLowResTargets(ctx, renderer, 1);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

// 比例变化之后重建低分辨率附件
static void applyOptions(const VulkanContext& ctx, LowResRenderer& renderer, const LowResOptions& options)
{
    const uint32_t divisor = ratioDivisor(options.ratio);
    if (renderer.lowResWidth == std::max(1u, ctx.swapchainExtent.width / divisor) &&
        renderer.lowResHeight == std::max(1u, ctx.swapchainExtent.height / divisor)) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));
    destroyLowResTargets(ctx, renderer);
    createLowResTargets(ctx, renderer, divisor);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, LowResRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const LowResSceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    LowResFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    applyOptions(ctx, renderer, input.options);

    outStatistics.gpuMilliseconds = 0.0;
    if (renderer.timestampsSupported && frame.timestampsValid) {
        uint64_t timestamps[2] = { 0, 0 };
        VK_CHECK(vkGetQueryPoolResults(ctx.device, frame.timestampPool, 0, 2, sizeof(timestamps), timestamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        outStatistics.gpuMilliseconds = static_cast<double>(timestamps[1] - timestamps[0]) *
                                        static_cast<double>(renderer.timestampPeriodNanoseconds) / 1000000.0;
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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(LowResSceneUniform));

    const double recordBeginStart = nowSeconds();

    VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    if (renderer.timestampsSupported) {
        vkCmdResetQueryPool(frame.commandBuffer, frame.timestampPool, 0, 2);
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool, 0);
    }

    outStatistics.cpuRecordBeginMilliseconds = (nowSeconds() - recordBeginStart) * 1000.0;

    VkViewport fullViewport = {};
    fullViewport.width = static_cast<float>(ctx.swapchainExtent.width);
    fullViewport.height = static_cast<float>(ctx.swapchainExtent.height);
    fullViewport.maxDepth = 1.0f;

    VkViewport lowViewport = {};
    lowViewport.width = static_cast<float>(renderer.lowResWidth);
    lowViewport.height = static_cast<float>(renderer.lowResHeight);
    lowViewport.maxDepth = 1.0f;

    VkRect2D fullScissor = {};
    fullScissor.extent = ctx.swapchainExtent;
    VkRect2D lowScissor = {};
    lowScissor.extent = { renderer.lowResWidth, renderer.lowResHeight };

    const uint32_t depthMode = input.options.depthMode;
    uint32_t drawCallCount = 0;

    // 场景通道
    const double sceneStart = nowSeconds();

    VkClearValue sceneClearValues[3] = {};
    sceneClearValues[0].color.float32[0] = 0.012f;
    sceneClearValues[0].color.float32[1] = 0.014f;
    sceneClearValues[0].color.float32[2] = 0.020f;
    sceneClearValues[0].color.float32[3] = 1.0f;
    sceneClearValues[1].color.float32[2] = 1.0f;  // 背景的法线取朝向相机的方向
    sceneClearValues[1].color.float32[3] = 1.0f;
    sceneClearValues[2].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo sceneBegin = {};
    sceneBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    sceneBegin.renderPass = renderer.sceneRenderPass;
    sceneBegin.framebuffer = renderer.sceneFramebuffer;
    sceneBegin.renderArea.extent = ctx.swapchainExtent;
    sceneBegin.clearValueCount = 3;
    sceneBegin.pClearValues = sceneClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &sceneBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &fullViewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &fullScissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePipeline);
    vkCmdDraw(frame.commandBuffer, 4, renderer.instanceCount, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;

    // 全分辨率的深度与法线要被后面的通道采样
    VkImageMemoryBarrier sceneRead[2] = {};
    const VkImage sceneReadImages[2] = { renderer.sceneDepth.image, renderer.sceneNormal.image };
    const VkImageAspectFlags sceneReadAspects[2] = { VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_COLOR_BIT };
    const VkImageLayout sceneReadOld[2] = { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    for (uint32_t i = 0; i < 2; ++i) {
        sceneRead[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sceneRead[i].oldLayout = sceneReadOld[i];
        sceneRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sceneRead[i].srcAccessMask = i == 0 ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                            : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sceneRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sceneRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneRead[i].image = sceneReadImages[i];
        sceneRead[i].subresourceRange.aspectMask = sceneReadAspects[i];
        sceneRead[i].subresourceRange.levelCount = 1;
        sceneRead[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, sceneRead);

    // 低分辨率层
    const double lowResStart = nowSeconds();

    if (depthMode == LOWRES_DEPTH_COPY) {
        VkRenderPassBeginInfo downgradeBegin = {};
        downgradeBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        downgradeBegin.renderPass = renderer.downgradeRenderPass;
        downgradeBegin.framebuffer = renderer.downgradeFramebuffer;
        downgradeBegin.renderArea.extent = lowScissor.extent;

        vkCmdBeginRenderPass(frame.commandBuffer, &downgradeBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &lowViewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &lowScissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.downgradePipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
    }

    const bool subpassGeometry = depthMode == LOWRES_DEPTH_SUBPASS;
    VkRenderPassBeginInfo glowBegin = {};
    glowBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    glowBegin.renderPass = subpassGeometry ? renderer.glowGeometryRenderPass : renderer.glowRenderPass;
    glowBegin.framebuffer = subpassGeometry ? renderer.glowGeometryFramebuffer : renderer.glowFramebuffer;
    glowBegin.renderArea.extent = lowScissor.extent;

    vkCmdBeginRenderPass(frame.commandBuffer, &glowBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &lowViewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &lowScissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      subpassGeometry ? renderer.glowGeometryPipeline : renderer.glowPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordLowResMilliseconds = (nowSeconds() - lowResStart) * 1000.0;

    // 合成通道要读场景颜色、低分辨率几何与低分辨率光斑。
    // 低分辨率几何在重新取的模式下没有任何通道写过它，布局停留在只读，屏障按这个事实声明
    const bool geometryWritten = depthMode != LOWRES_DEPTH_RECONSTRUCT;
    VkImageMemoryBarrier compositeRead[3] = {};
    VkImage compositeImages[3] = { renderer.sceneColor.image, renderer.lowGeometry.image,
                                   renderer.glowColor.image };
    const uint32_t compositeCount = 3;
    for (uint32_t i = 0; i < compositeCount; ++i) {
        const bool isGeometry = i == 1;
        const bool written = !isGeometry || geometryWritten;
        compositeRead[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        compositeRead[i].oldLayout = written ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        compositeRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        compositeRead[i].srcAccessMask = written ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        compositeRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        compositeRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        compositeRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        compositeRead[i].image = compositeImages[i];
        compositeRead[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        compositeRead[i].subresourceRange.levelCount = 1;
        compositeRead[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, compositeCount,
                         compositeRead);

    const double compositeStart = nowSeconds();

    VkRenderPassBeginInfo outputBegin = {};
    outputBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    outputBegin.renderPass = renderer.outputRenderPass;
    outputBegin.framebuffer = renderer.outputFramebuffers[imageIndex];
    outputBegin.renderArea.extent = ctx.swapchainExtent;
    outputBegin.clearValueCount = 0;

    vkCmdBeginRenderPass(frame.commandBuffer, &outputBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &fullViewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &fullScissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.compositePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.compositePipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordCompositeMilliseconds = (nowSeconds() - compositeStart) * 1000.0;

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
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool, 1);
    }

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
    outStatistics.lowResWidth = renderer.lowResWidth;
    outStatistics.lowResHeight = renderer.lowResHeight;

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
