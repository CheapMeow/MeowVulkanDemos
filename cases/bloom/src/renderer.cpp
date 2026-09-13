#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat HDR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

const char* bloomThresholdName(uint32_t threshold)
{
    if (threshold == BLOOM_THRESHOLD_HARD) {
        return "hard";
    }
    if (threshold == BLOOM_THRESHOLD_SOFT) {
        return "soft";
    }
    return "none";
}

const char* bloomChainName(uint32_t chain)
{
    if (chain == BLOOM_CHAIN_KAWASE) {
        return "kawase";
    }
    if (chain == BLOOM_CHAIN_MULTI) {
        return "multi";
    }
    return "gaussian";
}

const char* bloomOrderName(uint32_t order)
{
    return order == BLOOM_ORDER_TONEMAP_FIRST ? "tonemap_first" : "aa_first";
}

static void createColorRenderPass(const VulkanContext& ctx, VkFormat format, bool load, bool toPresent,
                                  VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // 叠加那一趟要保留本级原有的内容，初始布局必须是只读；其余的趟内容都可以丢弃
    attachment.initialLayout =
        load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout =
        toPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &outRenderPass));
}

static VkExtent2D levelExtent(const VulkanContext& ctx, uint32_t level)
{
    VkExtent2D extent = {};
    extent.width = std::max(1u, ctx.swapchainExtent.width >> (level + 1));
    extent.height = std::max(1u, ctx.swapchainExtent.height >> (level + 1));
    return extent;
}

static void createSwapchainTargets(const VulkanContext& ctx, BloomRenderer& renderer)
{
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, HDR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.hdrScene);
    for (uint32_t i = 0; i < BLOOM_MAX_LEVELS; ++i) {
        const VkExtent2D extent = levelExtent(ctx, i);
        createAttachmentTexture(ctx, extent.width, extent.height, HDR_FORMAT, usage,
                                VK_IMAGE_ASPECT_COLOR_BIT, renderer.levels[i]);
    }
    const VkExtent2D firstExtent = levelExtent(ctx, 0);
    createAttachmentTexture(ctx, firstExtent.width, firstExtent.height, HDR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.bloomResult);

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.layers = 1;

    VkImageView sceneViews[1] = { renderer.hdrScene.view };
    framebufferInfo.renderPass = renderer.writeRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = sceneViews;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sceneFramebuffer));

    for (uint32_t i = 0; i < BLOOM_MAX_LEVELS; ++i) {
        const VkExtent2D extent = levelExtent(ctx, i);
        VkImageView views[1] = { renderer.levels[i].view };
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.levelFramebuffers[i]));
    }

    VkImageView combineViews[1] = { renderer.bloomResult.view };
    framebufferInfo.pAttachments = combineViews;
    framebufferInfo.width = firstExtent.width;
    framebufferInfo.height = firstExtent.height;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.combineFramebuffer));

    renderer.outputFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView views[1] = { ctx.swapchainImageViews[i] };
        framebufferInfo.renderPass = renderer.outputRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = ctx.swapchainExtent.width;
        framebufferInfo.height = ctx.swapchainExtent.height;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.outputFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, BloomRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    vkDestroyFramebuffer(ctx.device, renderer.combineFramebuffer, nullptr);
    renderer.combineFramebuffer = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < BLOOM_MAX_LEVELS; ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.levelFramebuffers[i], nullptr);
        renderer.levelFramebuffers[i] = VK_NULL_HANDLE;
        destroyTexture(ctx, renderer.levels[i]);
    }
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.sceneFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.bloomResult);
    destroyTexture(ctx, renderer.hdrScene);
}

static void createDescriptorLayouts(const VulkanContext& ctx, BloomRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.textureSetLayout));

    VkDescriptorSetLayoutBinding combineBindings[BLOOM_MAX_LEVELS] = {};
    for (uint32_t i = 0; i < BLOOM_MAX_LEVELS; ++i) {
        combineBindings[i].binding = i;
        combineBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        combineBindings[i].descriptorCount = 1;
        combineBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo combineLayoutInfo = {};
    combineLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    combineLayoutInfo.bindingCount = BLOOM_MAX_LEVELS;
    combineLayoutInfo.pBindings = combineBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &combineLayoutInfo, nullptr,
                                         &renderer.combineSetLayout));

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 32;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 128;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeDescriptors(const VulkanContext& ctx, BloomRenderer& renderer, uint32_t index)
{
    BloomFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);

    writeBufferDescriptor(ctx, frame.thresholdSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);
    writeImageDescriptor(ctx, frame.thresholdSet, 1, renderer.hdrScene.view, renderer.linearSampler);

    for (uint32_t i = 1; i < BLOOM_MAX_LEVELS; ++i) {
        writeBufferDescriptor(ctx, frame.downSets[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              frame.uniformBuffer);
        writeImageDescriptor(ctx, frame.downSets[i], 1, renderer.levels[i - 1].view,
                             renderer.linearSampler);
    }
    for (uint32_t i = 0; i + 1 < BLOOM_MAX_LEVELS; ++i) {
        writeBufferDescriptor(ctx, frame.upSets[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              frame.uniformBuffer);
        writeImageDescriptor(ctx, frame.upSets[i], 1, renderer.levels[i + 1].view, renderer.linearSampler);
    }

    for (uint32_t i = 0; i < BLOOM_MAX_LEVELS; ++i) {
        writeImageDescriptor(ctx, frame.combineSet, i, renderer.levels[i].view, renderer.linearSampler);
    }

    writeBufferDescriptor(ctx, frame.presentSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeImageDescriptor(ctx, frame.presentSet, 1, renderer.hdrScene.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.presentSet, 2, renderer.levels[0].view, renderer.linearSampler);
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

static VkPipeline createFullscreenPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                           VkRenderPass renderPass, const char* fragmentShader,
                                           bool additive)
{
    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
    VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(fragmentShader));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);

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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (additive) {
        // 上采样叠加：本级原有的内容加上上一级放大后的结果
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

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

void createRenderer(const VulkanContext& ctx, BloomRenderer& renderer)
{
    renderer = BloomRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    const unsigned char black[4] = { 0, 0, 0, 255 };
    createTextureFromRgba(ctx, 1, 1, black, false, renderer.blackTexture, nullptr);

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

    createColorRenderPass(ctx, HDR_FORMAT, false, false, renderer.writeRenderPass);
    createColorRenderPass(ctx, HDR_FORMAT, true, false, renderer.blendRenderPass);
    createColorRenderPass(ctx, ctx.swapchainFormat, false, true, renderer.outputRenderPass);

    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.textureSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.presentPipelineLayout));

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 4;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.combineSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.combinePipelineLayout));

    renderer.scenePipeline = createFullscreenPipeline(ctx, renderer.scenePipelineLayout,
                                                      renderer.writeRenderPass,
                                                      DEMO_SHADER_DIR "/scene.frag.spv", false);
    renderer.downsamplePipeline = createFullscreenPipeline(ctx, renderer.scenePipelineLayout,
                                                          renderer.writeRenderPass,
                                                          DEMO_SHADER_DIR "/downsample.frag.spv", false);
    renderer.upsamplePipeline = createFullscreenPipeline(ctx, renderer.scenePipelineLayout,
                                                        renderer.blendRenderPass,
                                                        DEMO_SHADER_DIR "/upsample.frag.spv", true);
    renderer.combinePipeline = createFullscreenPipeline(ctx, renderer.combinePipelineLayout,
                                                       renderer.writeRenderPass,
                                                       DEMO_SHADER_DIR "/combine.frag.spv", false);
    renderer.presentPipeline = createFullscreenPipeline(ctx, renderer.presentPipelineLayout,
                                                       renderer.outputRenderPass,
                                                       DEMO_SHADER_DIR "/present.frag.spv", false);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        BloomFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(BloomSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.uniformBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.thresholdSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.presentSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        for (uint32_t level = 0; level < BLOOM_MAX_LEVELS; ++level) {
            frame.downSets[level] =
                allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
            frame.upSets[level] =
                allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        }
        frame.combineSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.combineSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, BloomRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        BloomFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.combinePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.upsamplePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.downsamplePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scenePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.combinePipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.presentPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.combineSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.textureSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.blendRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.writeRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyTexture(ctx, renderer.blackTexture);
}

void recreateSwapchainTargets(const VulkanContext& ctx, BloomRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, BloomRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const BloomSceneUniform& uniform, FrameStatistics& outStatistics)
{
    BloomFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

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

    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(BloomSceneUniform));

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

    uint32_t drawCallCount = 0;
    uint32_t renderPassCount = 0;

    VkViewport viewport = {};
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};

    const double sceneStart = nowSeconds();
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    scissor.extent = ctx.swapchainExtent;

    VkRenderPassBeginInfo sceneBegin = {};
    sceneBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    sceneBegin.renderPass = renderer.writeRenderPass;
    sceneBegin.framebuffer = renderer.sceneFramebuffer;
    sceneBegin.renderArea.extent = ctx.swapchainExtent;
    sceneBegin.clearValueCount = 0;

    vkCmdBeginRenderPass(frame.commandBuffer, &sceneBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);
    ++renderPassCount;
    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;

    const double chainStart = nowSeconds();
    const uint32_t levels = std::max(1u, std::min(input.options.levels, 5u));
    const bool bloomEnabled = input.options.threshold != BLOOM_THRESHOLD_NONE;
    const bool multiChain = input.options.chain == BLOOM_CHAIN_MULTI;

    // 第一趟：取出亮部并降到第一级。不做泛光时也跑这一趟，让第一级贴图有一个确定的布局
    {
        VkViewport levelViewport = {};
        levelViewport.width = static_cast<float>(renderer.levels[0].width);
        levelViewport.height = static_cast<float>(renderer.levels[0].height);
        levelViewport.maxDepth = 1.0f;
        VkRect2D levelScissor = {};
        levelScissor.extent = { renderer.levels[0].width, renderer.levels[0].height };

        VkRenderPassBeginInfo thresholdBegin = {};
        thresholdBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        thresholdBegin.renderPass = renderer.writeRenderPass;
        thresholdBegin.framebuffer = renderer.levelFramebuffers[0];
        thresholdBegin.renderArea.extent = levelScissor.extent;
        thresholdBegin.clearValueCount = 0;

        vkCmdBeginRenderPass(frame.commandBuffer, &thresholdBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &levelViewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &levelScissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.scenePipelineLayout, 0, 1, &frame.thresholdSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.downsamplePipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
        ++renderPassCount;
    }

    if (bloomEnabled) {
        VkViewport levelViewport = {};
        levelViewport.maxDepth = 1.0f;
        VkRect2D levelScissor = {};

        if (!multiChain) {
            for (uint32_t i = 1; i < levels; ++i) {
                levelViewport.width = static_cast<float>(renderer.levels[i].width);
                levelViewport.height = static_cast<float>(renderer.levels[i].height);
                levelScissor.extent = { renderer.levels[i].width, renderer.levels[i].height };

                VkRenderPassBeginInfo downBegin = {};
                downBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                downBegin.renderPass = renderer.writeRenderPass;
                downBegin.framebuffer = renderer.levelFramebuffers[i];
                downBegin.renderArea.extent = levelScissor.extent;

                vkCmdBeginRenderPass(frame.commandBuffer, &downBegin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(frame.commandBuffer, 0, 1, &levelViewport);
                vkCmdSetScissor(frame.commandBuffer, 0, 1, &levelScissor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        renderer.scenePipelineLayout, 0, 1, &frame.downSets[i], 0, nullptr);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  renderer.downsamplePipeline);
                vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
                ++drawCallCount;
                vkCmdEndRenderPass(frame.commandBuffer);
                ++renderPassCount;
            }

            for (uint32_t i = levels - 1; i > 0; --i) {
                const uint32_t target = i - 1;
                levelViewport.width = static_cast<float>(renderer.levels[target].width);
                levelViewport.height = static_cast<float>(renderer.levels[target].height);
                levelScissor.extent = { renderer.levels[target].width, renderer.levels[target].height };

                VkRenderPassBeginInfo upBegin = {};
                upBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                upBegin.renderPass = renderer.blendRenderPass;
                upBegin.framebuffer = renderer.levelFramebuffers[target];
                upBegin.renderArea.extent = levelScissor.extent;

                vkCmdBeginRenderPass(frame.commandBuffer, &upBegin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(frame.commandBuffer, 0, 1, &levelViewport);
                vkCmdSetScissor(frame.commandBuffer, 0, 1, &levelScissor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        renderer.scenePipelineLayout, 0, 1, &frame.upSets[target], 0, nullptr);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  renderer.upsamplePipeline);
                vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
                ++drawCallCount;
                vkCmdEndRenderPass(frame.commandBuffer);
                ++renderPassCount;
            }
        } else {
            for (uint32_t i = 1; i < BLOOM_MAX_LEVELS; ++i) {
                levelViewport.width = static_cast<float>(renderer.levels[i].width);
                levelViewport.height = static_cast<float>(renderer.levels[i].height);
                levelScissor.extent = { renderer.levels[i].width, renderer.levels[i].height };

                VkRenderPassBeginInfo downBegin = {};
                downBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                downBegin.renderPass = renderer.writeRenderPass;
                downBegin.framebuffer = renderer.levelFramebuffers[i];
                downBegin.renderArea.extent = levelScissor.extent;

                vkCmdBeginRenderPass(frame.commandBuffer, &downBegin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(frame.commandBuffer, 0, 1, &levelViewport);
                vkCmdSetScissor(frame.commandBuffer, 0, 1, &levelScissor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        renderer.scenePipelineLayout, 0, 1, &frame.downSets[i], 0, nullptr);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  renderer.downsamplePipeline);
                vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
                ++drawCallCount;
                vkCmdEndRenderPass(frame.commandBuffer);
                ++renderPassCount;
            }

            // 一次采样多个层级，把整条链合成到一张贴图上
            levelViewport.width = static_cast<float>(renderer.bloomResult.width);
            levelViewport.height = static_cast<float>(renderer.bloomResult.height);
            levelScissor.extent = { renderer.bloomResult.width, renderer.bloomResult.height };

            VkRenderPassBeginInfo combineBegin = {};
            combineBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            combineBegin.renderPass = renderer.writeRenderPass;
            combineBegin.framebuffer = renderer.combineFramebuffer;
            combineBegin.renderArea.extent = levelScissor.extent;

            vkCmdBeginRenderPass(frame.commandBuffer, &combineBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &levelViewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &levelScissor);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    renderer.combinePipelineLayout, 0, 1, &frame.combineSet, 0, nullptr);
            const float combineParams[4] = { input.options.intensity, 0.0f, 0.0f, 0.0f };
            vkCmdPushConstants(frame.commandBuffer, renderer.combinePipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(combineParams), combineParams);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.combinePipeline);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            ++drawCallCount;
            vkCmdEndRenderPass(frame.commandBuffer);
            ++renderPassCount;
        }
    }
    outStatistics.cpuRecordChainMilliseconds = (nowSeconds() - chainStart) * 1000.0;

    const double presentStart = nowSeconds();
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
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
                            renderer.presentPipelineLayout, 0, 1, &frame.presentSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.presentPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;
    vkCmdEndRenderPass(frame.commandBuffer);
    ++renderPassCount;
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
    outStatistics.renderPassCount = renderPassCount;

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
