#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

// 多重采样颜色的格式：半精度浮点
static const VkFormat MSAA_COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

static const uint32_t SAMPLE_OPTIONS[ALPHA_SAMPLE_OPTION_COUNT] = { 1, 2, 4, 8 };

const uint32_t* alphaSampleOptions()
{
    return SAMPLE_OPTIONS;
}

const char* alphaModeName(uint32_t mode)
{
    if (mode == ALPHA_MODE_BLEND) {
        return "blend";
    }
    if (mode == ALPHA_MODE_TO_COVERAGE) {
        return "coverage";
    }
    return "test";
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

// 几何通道：多重采样的颜色与深度附件
static void createGeometryRenderPass(const VulkanContext& ctx, AlphaRenderer& renderer)
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

// 色调映射通道：写交换链图像
static void createTonemapRenderPass(const VulkanContext& ctx, AlphaRenderer& renderer)
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

static void createSwapchainTargets(const VulkanContext& ctx, AlphaRenderer& renderer)
{
    const VkSampleCountFlagBits samples = sampleCountFlag(renderer.sampleCount);

    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, MSAA_COLOR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

static void destroySwapchainTargets(const VulkanContext& ctx, AlphaRenderer& renderer)
{
    vkDestroyFramebuffer(ctx.device, renderer.geometryFramebuffer, nullptr);
    renderer.geometryFramebuffer = VK_NULL_HANDLE;
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

static void createDescriptorLayouts(const VulkanContext& ctx, AlphaRenderer& renderer)
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

static void createDescriptorPool(const VulkanContext& ctx, AlphaRenderer& renderer)
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

enum GeometryPipelineKind {
    GEOMETRY_PIPELINE_TEST = 0,
    GEOMETRY_PIPELINE_BLEND = 1,
    GEOMETRY_PIPELINE_COVERAGE = 2,
};

// 三条几何管线共用顶点展开与着色器，差别只在深度写入、混合与 alpha to coverage 状态
static void createGeometryPipeline(const VulkanContext& ctx, AlphaRenderer& renderer,
                                   GeometryPipelineKind kind, VkPipelineShaderStageCreateInfo* stages)
{
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
    // alpha to coverage 只在采样数大于一时成立，单采样下这一位必须保持关闭
    multisample.alphaToCoverageEnable =
        kind == GEOMETRY_PIPELINE_COVERAGE && renderer.sampleCount > 1 ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (kind == GEOMETRY_PIPELINE_BLEND) {
        // 半透明不参与深度，既不测试也不写入，完全按提交顺序叠加
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
    } else {
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (kind == GEOMETRY_PIPELINE_BLEND) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    pipelineInfo.layout = renderer.geometryPipelineLayout;
    pipelineInfo.renderPass = renderer.geometryRenderPass;
    pipelineInfo.subpass = 0;

    VkPipeline* target = &renderer.geometryTestPipeline;
    if (kind == GEOMETRY_PIPELINE_BLEND) {
        target = &renderer.geometryBlendPipeline;
    } else if (kind == GEOMETRY_PIPELINE_COVERAGE) {
        target = &renderer.geometryCoveragePipeline;
    }
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, target));
}

static void createGeometryPipelines(const VulkanContext& ctx, AlphaRenderer& renderer)
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

    createGeometryPipeline(ctx, renderer, GEOMETRY_PIPELINE_TEST, stages);
    createGeometryPipeline(ctx, renderer, GEOMETRY_PIPELINE_BLEND, stages);
    createGeometryPipeline(ctx, renderer, GEOMETRY_PIPELINE_COVERAGE, stages);

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

static void createTonemapPipeline(const VulkanContext& ctx, AlphaRenderer& renderer)
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

// 低差异序列，用来把植被铺开
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

// 场景：一批相互交叠的植被卡片，alpha 全部来自程序生成的镂空掩码。
// 逻辑深度用于排序：数值越大越远
static void buildShapes(std::vector<AlphaShape>& shapes)
{
    shapes.clear();
    for (int i = 0; i < 20; ++i) {
        const uint32_t index = static_cast<uint32_t>(i);
        const float x = -0.92f + 1.7f * haltonSequence(index, 2);
        const float y = -0.92f + 1.7f * haltonSequence(index, 3);
        const float size = 0.26f + 0.24f * haltonSequence(index, 5);
        const float depth = y + 0.15f * haltonSequence(index, 11);
        const float phase = 6.2831853f * haltonSequence(index, 13);
        shapes.push_back({ glm::vec4(x, y, size, 1.0f),
                           glm::vec4(depth, phase, static_cast<float>(i), 0.0f) });
    }
}

void createRenderer(const VulkanContext& ctx, AlphaRenderer& renderer, uint32_t sampleCount)
{
    renderer = AlphaRenderer();
    renderer.sampleCount = sampleCount;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<AlphaShape> shapes;
    buildShapes(shapes);
    renderer.shapeCount = static_cast<uint32_t>(shapes.size());

    // 形状缓冲里放两份：前半由远到近，后半由近到远。每块形状两个 vec4
    std::vector<AlphaShape> farToNear = shapes;
    std::vector<AlphaShape> nearToFar = shapes;
    std::stable_sort(farToNear.begin(), farToNear.end(),
                     [](const AlphaShape& a, const AlphaShape& b) { return a.params.x > b.params.x; });
    std::stable_sort(nearToFar.begin(), nearToFar.end(),
                     [](const AlphaShape& a, const AlphaShape& b) { return a.params.x < b.params.x; });
    farToNear.insert(farToNear.end(), nearToFar.begin(), nearToFar.end());

    const VkDeviceSize shapeBytes = sizeof(AlphaShape) * farToNear.size();
    createBuffer(ctx, shapeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.shapeBuffer);
    uploadBufferData(ctx, renderer.shapeBuffer, farToNear.data(), shapeBytes);

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
    createTonemapRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createGeometryPipelines(ctx, renderer);
    createTonemapPipeline(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    renderer.samplerSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.samplerSetLayout);
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        AlphaFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(AlphaSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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

void destroyRenderer(const VulkanContext& ctx, AlphaRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        AlphaFrameResources& frame = renderer.frames[i];
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
    vkDestroyPipeline(ctx.device, renderer.geometryCoveragePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryBlendPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryTestPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.geometryPipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.samplerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.tonemapRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.geometryRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.sampler, nullptr);
    destroyBuffer(ctx, renderer.shapeBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, AlphaRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);
}

// 采样数改动之后重建：采样数影响渲染通道、附件与全部管线
static void applyOptions(const VulkanContext& ctx, AlphaRenderer& renderer, const AlphaOptions& options)
{
    if (renderer.sampleCount == options.sampleCount) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    vkDestroyPipeline(ctx.device, renderer.tonemapPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.tonemapPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryCoveragePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryBlendPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryTestPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.geometryPipelineLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.tonemapRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.geometryRenderPass, nullptr);

    renderer.sampleCount = options.sampleCount;

    createGeometryRenderPass(ctx, renderer);
    createTonemapRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createGeometryPipelines(ctx, renderer);
    createTonemapPipeline(ctx, renderer);
    writeImageDescriptor(ctx, renderer.samplerSet, 0, renderer.resolveTexture.view, renderer.sampler);
}

bool drawFrame(const VulkanContext& ctx, AlphaRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const AlphaSceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    AlphaFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(AlphaSceneUniform));

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
    geometryClearValues[0].color.float32[0] = sceneUniform.colorParams.x;
    geometryClearValues[0].color.float32[1] = sceneUniform.colorParams.y;
    geometryClearValues[0].color.float32[2] = sceneUniform.colorParams.z;
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

    // 三种处理方式各有一条管线；alpha to coverage 需要采样数大于一，单采样下按 alpha test 处理
    uint32_t mode = input.options.mode;
    if (mode == ALPHA_MODE_TO_COVERAGE && renderer.sampleCount == 1) {
        mode = ALPHA_MODE_TEST;
    }

    VkPipeline pipeline = renderer.geometryTestPipeline;
    if (mode == ALPHA_MODE_BLEND) {
        pipeline = renderer.geometryBlendPipeline;
    } else if (mode == ALPHA_MODE_TO_COVERAGE) {
        pipeline = renderer.geometryCoveragePipeline;
    }

    // 不透明方式按由近到远绘制，前面的片元先把深度写下去，后面的被提前剔除；
    // 混合方式按由远到近绘制，才能得到正确的叠加结果
    const uint32_t firstInstance = mode == ALPHA_MODE_BLEND ? 0 : renderer.shapeCount;

    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.geometryPipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(frame.commandBuffer, 4, renderer.shapeCount, 0, firstInstance);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordGeometryPassMilliseconds = (nowSeconds() - geometryStart) * 1000.0;

    const double resolveStart = nowSeconds();
    // 几何通道把颜色留在颜色附件布局，解析前转到传输源
    VkImageMemoryBarrier afterGeometry = {};
    afterGeometry.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    afterGeometry.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    afterGeometry.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    afterGeometry.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    afterGeometry.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    afterGeometry.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterGeometry.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterGeometry.image = renderer.msaaColor.image;
    afterGeometry.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    afterGeometry.subresourceRange.levelCount = 1;
    afterGeometry.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &afterGeometry);

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

    if (renderer.sampleCount == 1) {
        // 单采样没有可解析的采样点，直接拷贝
        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent.width = ctx.swapchainExtent.width;
        copyRegion.extent.height = ctx.swapchainExtent.height;
        copyRegion.extent.depth = 1;
        vkCmdCopyImage(frame.commandBuffer, renderer.msaaColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       renderer.resolveTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    } else {
        VkImageResolve resolveRegion = {};
        resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveRegion.srcSubresource.layerCount = 1;
        resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveRegion.dstSubresource.layerCount = 1;
        resolveRegion.extent.width = ctx.swapchainExtent.width;
        resolveRegion.extent.height = ctx.swapchainExtent.height;
        resolveRegion.extent.depth = 1;
        vkCmdResolveImage(frame.commandBuffer, renderer.msaaColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          renderer.resolveTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                          &resolveRegion);
    }

    VkImageMemoryBarrier resolveRead = resolveWrite;
    resolveRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    resolveRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    resolveRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    resolveRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &resolveRead);
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
