#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <cstddef>
#include <cstring>
#include <vector>

// 多重采样颜色的格式：半精度浮点，背景亮度可以超过 1
static const VkFormat MSAA_COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

static const uint32_t SAMPLE_OPTIONS[MSAA_SAMPLE_OPTION_COUNT] = { 1, 2, 4, 8 };

const uint32_t* msaaSampleOptions()
{
    return SAMPLE_OPTIONS;
}

const char* msaaResolveModeName(uint32_t resolveMode)
{
    return resolveMode == RESOLVE_CUSTOM ? "custom" : "hardware";
}

static VkSampleCountFlagBits sampleCountFlag(uint32_t sampleCount)
{
    if (sampleCount >= 8) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if (sampleCount >= 4) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if (sampleCount >= 2) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

// 几何通道：多重采样的颜色与深度附件，颜色在通道结束时保持颜色附件布局，
// 之后按解析方式用一次屏障转到传输源或只读布局
static void createGeometryRenderPass(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = MSAA_COLOR_FORMAT;
    attachments[0].samples = sampleCountFlag(renderer.sampleCount);
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = sampleCountFlag(renderer.sampleCount);
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.geometryRenderPass));
}

// 自定义解析通道：写单采样的解析目标
static void createResolveRenderPass(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = MSAA_COLOR_FORMAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.resolveRenderPass));
}

// 色调映射通道：写交换链图像
static void createTonemapRenderPass(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = ctx.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    // 全屏三角形会覆盖每一个像素，不需要清除
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.tonemapRenderPass));
}

static void createSwapchainTargets(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    const VkSampleCountFlagBits samples = sampleCountFlag(renderer.sampleCount);

    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, MSAA_COLOR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.msaaColor, samples);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.msaaDepth, samples);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, MSAA_COLOR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.resolveTexture, VK_SAMPLE_COUNT_1_BIT);

    VkImageView geometryViews[2] = { renderer.msaaColor.view, renderer.msaaDepth.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.geometryRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = geometryViews;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.geometryFramebuffer));

    VkImageView resolveViews[1] = { renderer.resolveTexture.view };
    framebufferInfo.renderPass = renderer.resolveRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = resolveViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.resolveFramebuffer));

    renderer.tonemapFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView views[1] = { ctx.swapchainImageViews[i] };
        framebufferInfo.renderPass = renderer.tonemapRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = views;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.tonemapFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    vkDestroyFramebuffer(ctx.device, renderer.geometryFramebuffer, nullptr);
    renderer.geometryFramebuffer = VK_NULL_HANDLE;
    vkDestroyFramebuffer(ctx.device, renderer.resolveFramebuffer, nullptr);
    renderer.resolveFramebuffer = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < renderer.tonemapFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.tonemapFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.tonemapFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.resolveTexture);
    destroyTexture(ctx, renderer.msaaDepth);
    destroyTexture(ctx, renderer.msaaColor);
}

static void createDescriptorLayouts(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkDescriptorSetLayoutBinding sceneBindings[3] = {};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[1].descriptorCount = 1;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    sceneBindings[2].binding = 2;
    sceneBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[2].descriptorCount = 1;
    sceneBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sceneLayoutInfo = {};
    sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sceneLayoutInfo.bindingCount = 3;
    sceneLayoutInfo.pBindings = sceneBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &sceneLayoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo samplerLayoutInfo = {};
    samplerLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    samplerLayoutInfo.bindingCount = 1;
    samplerLayoutInfo.pBindings = &samplerBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &samplerLayoutInfo, nullptr, &renderer.samplerSetLayout));
}

static void createDescriptorPool(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 16;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 8;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
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

// 几何管线：顶点直接给出裁剪空间坐标，没有顶点缓冲，形状由实例缓冲给出
static void createGeometryPipelines(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.geometryPipelineLayout));

    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/shape.vert.spv"));
    VkShaderModule fragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/shape.frag.spv"));

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
    multisample.rasterizationSamples = sampleCountFlag(renderer.sampleCount);

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // alpha to coverage 是管线状态，两套都建好，绘制时按开关选择
    for (int pass = 0; pass < 2; ++pass) {
        multisample.alphaToCoverageEnable = pass == 1 ? VK_TRUE : VK_FALSE;

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
        pipelineInfo.layout = renderer.geometryPipelineLayout;
        pipelineInfo.renderPass = renderer.geometryRenderPass;
        pipelineInfo.subpass = 0;

        VkPipeline* targetPipeline =
            pass == 1 ? &renderer.geometryCoveragePipeline : &renderer.geometryOpaquePipeline;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           targetPipeline));
    }

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

static void createResolvePipeline(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(int32_t);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.samplerSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.resolvePipelineLayout));

    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
    VkShaderModule fragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/resolve_custom.frag.spv"));

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
    pipelineInfo.layout = renderer.resolvePipelineLayout;
    pipelineInfo.renderPass = renderer.resolveRenderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &renderer.resolvePipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

static void createTonemapPipeline(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.samplerSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.tonemapPipelineLayout));

    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
    VkShaderModule fragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/tonemap.frag.spv"));

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
    pipelineInfo.layout = renderer.tonemapPipelineLayout;
    pipelineInfo.renderPass = renderer.tonemapRenderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &renderer.tonemapPipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

// 低差异序列，用来把形状铺开：形状之间留出空隙，背景才会露出来，
// 多重采样也才有边界可以抗锯齿
static float haltonSequence(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    uint32_t value = index + 1;
    while (value > 0) {
        fraction /= static_cast<float>(base);
        result += static_cast<float>(value % base) * fraction;
        value /= base;
    }
    return result;
}

// 三个子场景的形状：全屏三角、交叠的形状与镂空植被、高动态范围。
// 每个子场景在形状缓冲里占用连续一段，绘制时用 firstInstance 指向这一段
static void buildShapes(std::vector<ShapeData>& shapes, uint32_t* outOffsets, uint32_t* outCounts)
{
    shapes.clear();

    // 子场景 0：一块铺满视口的背景
    outOffsets[0] = static_cast<uint32_t>(shapes.size());
    shapes.push_back({ glm::vec4(-1.0f, -1.0f, 2.0f, 1.0f) });
    shapes.push_back({ glm::vec4(-1.0f, 1.0f, 2.0f, 1.0f) });
    outCounts[0] = static_cast<uint32_t>(shapes.size()) - outOffsets[0];

    // 子场景 1：一批相互交叠又留有空隙的形状，最后一个是镂空形状（透明度为负）
    outOffsets[1] = static_cast<uint32_t>(shapes.size());
    for (int i = 0; i < 24; ++i) {
        const uint32_t index = static_cast<uint32_t>(i);
        const float x = -0.85f + 1.55f * haltonSequence(index, 2);
        const float y = -0.85f + 1.55f * haltonSequence(index, 3);
        const float size = 0.10f + 0.16f * haltonSequence(index, 5);
        const float alpha = 0.2f + 0.7f * static_cast<float>(i) / 24.0f;
        shapes.push_back({ glm::vec4(x, y, size, alpha) });
    }
    shapes.push_back({ glm::vec4(-0.35f, -0.85f, 0.7f, -1.0f) });
    outCounts[1] = static_cast<uint32_t>(shapes.size()) - outOffsets[1];

    // 子场景 2：高动态范围，背景亮度由 uniform 给出，这里只放两块暗色形状
    outOffsets[2] = static_cast<uint32_t>(shapes.size());
    shapes.push_back({ glm::vec4(-0.9f, -0.2f, 0.7f, 0.05f) });
    shapes.push_back({ glm::vec4(0.1f, -0.3f, 0.9f, 0.02f) });
    outCounts[2] = static_cast<uint32_t>(shapes.size()) - outOffsets[2];
}

void createRenderer(const VulkanContext& ctx, MsaaRenderer& renderer, uint32_t sampleCount,
                    uint32_t resolveMode)
{
    renderer = MsaaRenderer();
    renderer.sampleCount = sampleCount;
    renderer.resolveMode = resolveMode;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<ShapeData> shapes;
    buildShapes(shapes, renderer.shapeOffsets, renderer.shapeCounts);

    const VkDeviceSize shapeBytes = sizeof(ShapeData) * shapes.size();
    createBuffer(ctx, shapeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.shapeBuffer);
    uploadBufferData(ctx, renderer.shapeBuffer, shapes.data(), shapeBytes);

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.sampler));

    createGeometryRenderPass(ctx, renderer);
    createResolveRenderPass(ctx, renderer);
    createTonemapRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createGeometryPipelines(ctx, renderer);
    createResolvePipeline(ctx, renderer);
    createTonemapPipeline(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    renderer.samplerSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.samplerSetLayout);
    renderer.resolveSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.samplerSetLayout);
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);
    writeImageDescriptor(ctx, renderer.resolveSet, 0, renderer.msaaColor.view, renderer.sampler);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        MsaaFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(MsaaSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);
        createBuffer(ctx, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.counterBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
        writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              renderer.shapeBuffer);
        writeBufferDescriptor(ctx, frame.sceneSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              frame.counterBuffer);

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

void destroyRenderer(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        MsaaFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.counterBuffer);
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.tonemapPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.tonemapPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.resolvePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.resolvePipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryCoveragePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryOpaquePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.geometryPipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.samplerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.tonemapRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.resolveRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.geometryRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.sampler, nullptr);
    destroyBuffer(ctx, renderer.shapeBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, MsaaRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    // 两张贴图都换了视图，两个描述符集都要改写
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);
    writeImageDescriptor(ctx, renderer.resolveSet, 0, renderer.msaaColor.view, renderer.sampler);
}

// 采样数与解析方式改动之后重建：采样数影响渲染通道、附件与全部管线，
// 解析方式只影响几何通道输出之后的屏障与是否跑自定义解析通道
static void applyOptions(const VulkanContext& ctx, MsaaRenderer& renderer, const MsaaOptions& options)
{
    const bool samplesChanged = renderer.sampleCount != options.sampleCount;
    if (!samplesChanged) {
        renderer.resolveMode = options.resolveMode;
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    vkDestroyPipeline(ctx.device, renderer.tonemapPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.tonemapPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.resolvePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.resolvePipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryCoveragePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryOpaquePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.geometryPipelineLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.tonemapRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.resolveRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.geometryRenderPass, nullptr);

    renderer.sampleCount = options.sampleCount;
    renderer.resolveMode = options.resolveMode;

    createGeometryRenderPass(ctx, renderer);
    createResolveRenderPass(ctx, renderer);
    createTonemapRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createGeometryPipelines(ctx, renderer);
    createResolvePipeline(ctx, renderer);
    createTonemapPipeline(ctx, renderer);
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);
    writeImageDescriptor(ctx, renderer.resolveSet, 0, renderer.msaaColor.view, renderer.sampler);
}

bool drawFrame(const VulkanContext& ctx, MsaaRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const MsaaSceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    MsaaFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    applyOptions(ctx, renderer, input.options);

    // 片元计数由上一帧写入，等栅栏之后再读
    uint32_t fragmentCount = 0;
    std::memcpy(&fragmentCount, frame.counterBuffer.mapped, sizeof(uint32_t));
    outStatistics.fragmentCount = fragmentCount;
    const uint32_t zero = 0;
    std::memcpy(frame.counterBuffer.mapped, &zero, sizeof(uint32_t));

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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(MsaaSceneUniform));

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

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    uint32_t drawCallCount = 0;

    const double geometryStart = nowSeconds();
    VkClearValue geometryClearValues[2] = {};
    geometryClearValues[0].color.float32[0] = 0.0f;
    geometryClearValues[0].color.float32[1] = 0.0f;
    geometryClearValues[0].color.float32[2] = 0.0f;
    geometryClearValues[0].color.float32[3] = 1.0f;
    geometryClearValues[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo geometryBegin = {};
    geometryBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geometryBegin.renderPass = renderer.geometryRenderPass;
    geometryBegin.framebuffer = renderer.geometryFramebuffer;
    geometryBegin.renderArea.extent = ctx.swapchainExtent;
    geometryBegin.clearValueCount = 2;
    geometryBegin.pClearValues = geometryClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &geometryBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    const uint32_t sceneIndex = std::min(input.options.sceneIndex, 2u);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.geometryPipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    // 子场景 2 是全屏背景，用不透明管线；其余两个子场景按开关选择
    const bool useCoverage = input.options.alphaToCoverage && renderer.sampleCount > 1 && sceneIndex != 2;
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      useCoverage ? renderer.geometryCoveragePipeline : renderer.geometryOpaquePipeline);
    vkCmdDraw(frame.commandBuffer, 4, renderer.shapeCounts[sceneIndex], 0, renderer.shapeOffsets[sceneIndex]);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordGeometryPassMilliseconds = (nowSeconds() - geometryStart) * 1000.0;

    const double resolveStart = nowSeconds();
    // 采样数为一的时候没有东西可以解析，直接把颜色附件拷到解析目标
    const bool singleSample = renderer.sampleCount == 1;
    const bool useCustomResolve = !singleSample && renderer.resolveMode == RESOLVE_CUSTOM;
    // 几何通道把颜色留在颜色附件布局，按解析方式转到传输源或只读布局
    VkImageMemoryBarrier afterGeometry = {};
    afterGeometry.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    afterGeometry.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    afterGeometry.newLayout = useCustomResolve ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    afterGeometry.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    afterGeometry.dstAccessMask =
        useCustomResolve ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    afterGeometry.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterGeometry.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterGeometry.image = renderer.msaaColor.image;
    afterGeometry.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    afterGeometry.subresourceRange.levelCount = 1;
    afterGeometry.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         useCustomResolve ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                          : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &afterGeometry);

    if (useCustomResolve) {
        VkImageMemoryBarrier resolveWrite = {};
        resolveWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveWrite.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveWrite.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolveWrite.srcAccessMask = 0;
        resolveWrite.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveWrite.image = renderer.resolveTexture.image;
        resolveWrite.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveWrite.subresourceRange.levelCount = 1;
        resolveWrite.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &resolveWrite);

        VkRenderPassBeginInfo resolveBegin = {};
        resolveBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        resolveBegin.renderPass = renderer.resolveRenderPass;
        resolveBegin.framebuffer = renderer.resolveFramebuffer;
        resolveBegin.renderArea.extent = ctx.swapchainExtent;

        vkCmdBeginRenderPass(frame.commandBuffer, &resolveBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.resolvePipeline);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.resolvePipelineLayout, 0, 1, &renderer.resolveSet, 0, nullptr);
        const int32_t sampleCount = static_cast<int32_t>(renderer.sampleCount);
        vkCmdPushConstants(frame.commandBuffer, renderer.resolvePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(sampleCount), &sampleCount);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
    } else {
        VkImageMemoryBarrier resolveWrite = {};
        resolveWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveWrite.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolveWrite.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveWrite.srcAccessMask = 0;
        resolveWrite.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveWrite.image = renderer.resolveTexture.image;
        resolveWrite.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveWrite.subresourceRange.levelCount = 1;
        resolveWrite.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &resolveWrite);

        VkImageResolve resolveRegion = {};
        resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveRegion.srcSubresource.layerCount = 1;
        resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveRegion.dstSubresource.layerCount = 1;
        resolveRegion.extent.width = ctx.swapchainExtent.width;
        resolveRegion.extent.height = ctx.swapchainExtent.height;
        resolveRegion.extent.depth = 1;

        if (singleSample) {
            // 单采样没有可解析的采样点，直接拷贝
            VkImageCopy copyRegion = {};
            copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.extent.width = ctx.swapchainExtent.width;
            copyRegion.extent.height = ctx.swapchainExtent.height;
            copyRegion.extent.depth = 1;
            vkCmdCopyImage(frame.commandBuffer, renderer.msaaColor.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.resolveTexture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        } else {
            vkCmdResolveImage(frame.commandBuffer, renderer.msaaColor.image,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.resolveTexture.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolveRegion);
        }

        VkImageMemoryBarrier resolveRead = resolveWrite;
        resolveRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        resolveRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &resolveRead);
    }
    outStatistics.cpuRecordResolveMilliseconds = (nowSeconds() - resolveStart) * 1000.0;

    const double tonemapStart = nowSeconds();
    VkRenderPassBeginInfo tonemapBegin = {};
    tonemapBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    tonemapBegin.renderPass = renderer.tonemapRenderPass;
    tonemapBegin.framebuffer = renderer.tonemapFramebuffers[imageIndex];
    tonemapBegin.renderArea.extent = ctx.swapchainExtent;
    tonemapBegin.clearValueCount = 0;

    vkCmdBeginRenderPass(frame.commandBuffer, &tonemapBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.tonemapPipeline);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.tonemapPipelineLayout, 0, 1, &renderer.samplerSet, 0, nullptr);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;

    outStatistics.cpuRecordTonemapMilliseconds = (nowSeconds() - tonemapStart) * 1000.0;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    vkCmdEndRenderPass(frame.commandBuffer);

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
