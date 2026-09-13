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
static const VkFormat OCCLUSION_FORMAT = VK_FORMAT_R16_SFLOAT;

const char* ssaoModeName(uint32_t mode)
{
    if (mode == SSAO_MODE_AMBIENT) {
        return "ambient";
    }
    if (mode == SSAO_MODE_ALL) {
        return "all";
    }
    return "off";
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

// 三排相互遮挡的板，相邻的板之间有深度台阶与法线台阶
static void buildSceneInstances(std::vector<glm::vec4>& instances, uint32_t& outCount)
{
    instances.clear();
    const uint32_t rows = 3;
    const uint32_t columns = 6;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const uint32_t index = row * columns + column;
            const float x = -1.5f + static_cast<float>(column) * 0.6f + (hash01(index * 3u + 1u) - 0.5f) * 0.1f;
            const float y = -0.9f + static_cast<float>(row) * 0.9f;
            const float z = -2.6f - 0.35f * static_cast<float>(index % 5);
            const float yaw = (column % 2 == 0 ? 1.0f : -1.0f) * (0.5f + 0.06f * static_cast<float>(row));
            const float shade = 0.55f + 0.45f * hash01(index * 7u + 3u);
            const float hue = static_cast<float>(index) / static_cast<float>(rows * columns);
            instances.push_back(glm::vec4(x, y, 0.3f, 0.42f));
            instances.push_back(glm::vec4(yaw, shade, z, hue));
        }
    }
    outCount = rows * columns;
}

// 一张四乘四的随机噪声，每个像素用它错开采样核的起始角度
static void buildNoisePixels(std::vector<unsigned char>& pixels, uint32_t size)
{
    pixels.resize(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const unsigned char value = static_cast<unsigned char>(hash01(y * size + x) * 255.0f);
            const size_t index = (static_cast<size_t>(y) * size + x) * 4;
            pixels[index + 0] = value;
            pixels[index + 1] = value;
            pixels[index + 2] = value;
            pixels[index + 3] = 255;
        }
    }
}

static void createSceneRenderPass(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    VkAttachmentDescription attachments[3] = {};
    const VkFormat formats[3] = { SCENE_COLOR_FORMAT, SCENE_NORMAL_FORMAT, SCENE_DEPTH_FORMAT };
    for (int i = 0; i < 3; ++i) {
        attachments[i].format = formats[i];
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = i == 2 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

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

static void createSingleColorRenderPass(const VulkanContext& ctx, VkFormat format, bool toPresent,
                                        VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout =
        toPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &outRenderPass));
}

static void createSwapchainTargets(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    const VkImageUsageFlags attachmentUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_COLOR_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneColor);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_NORMAL_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneNormal);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.sceneDepth);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, OCCLUSION_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.occlusion);

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

    VkImageView occlusionViews[1] = { renderer.occlusion.view };
    framebufferInfo.renderPass = renderer.occlusionRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = occlusionViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.occlusionFramebuffer));

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

static void destroySwapchainTargets(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    vkDestroyFramebuffer(ctx.device, renderer.occlusionFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.occlusionFramebuffer = VK_NULL_HANDLE;
    renderer.sceneFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.occlusion);
    destroyTexture(ctx, renderer.sceneDepth);
    destroyTexture(ctx, renderer.sceneNormal);
    destroyTexture(ctx, renderer.sceneColor);
}

static void createDescriptorLayouts(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    for (uint32_t i = 2; i <= 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
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
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

// 三个描述符集各自把用得到的贴图写进约定的绑定：遮蔽通道把噪声写在第 4 位，
// 合成通道把场景颜色写在第 4 位、遮蔽量写在第 5 位
static void writeDescriptors(const VulkanContext& ctx, SsaoRenderer& renderer, uint32_t index)
{
    SsaoFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.sceneSet, 2, renderer.sceneDepth.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 3, renderer.sceneNormal.view, renderer.nearestSampler);

    writeBufferDescriptor(ctx, frame.occlusionSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.occlusionSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.occlusionSet, 2, renderer.sceneDepth.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.occlusionSet, 3, renderer.sceneNormal.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.occlusionSet, 4, renderer.noiseTexture.view, renderer.nearestSampler);

    writeBufferDescriptor(ctx, frame.compositeSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.compositeSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.compositeSet, 2, renderer.sceneDepth.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.compositeSet, 3, renderer.sceneNormal.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.compositeSet, 4, renderer.sceneColor.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.compositeSet, 5, renderer.occlusion.view, renderer.linearSampler);
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

static void fillCommonStates(VkPipelineVertexInputStateCreateInfo& vertexInput,
                             VkPipelineInputAssemblyStateCreateInfo& inputAssembly,
                             VkPipelineViewportStateCreateInfo& viewportState,
                             VkPipelineRasterizationStateCreateInfo& rasterization,
                             VkPipelineMultisampleStateCreateInfo& multisample,
                             VkPipelineDepthStencilStateCreateInfo& depthStencil,
                             VkPipelineDynamicStateCreateInfo& dynamicState, bool depthTest)
{
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

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
    depthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    static VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
}

static void createPipelines(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.compositePipelineLayout));

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    // 场景管线
    {
        VkShaderModule vertexModule =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.vert.spv"));
        VkShaderModule fragmentModule =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.frag.spv"));
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
        fillCommonStates(vertexInput, inputAssembly, viewportState, rasterization, multisample,
                         depthStencil, dynamicState, true);

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 2;
        colorBlend.pAttachments = blendAttachments;

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

    // 遮蔽管线与合成管线：全屏三角形，各一张颜色附件
    const char* fragmentShaders[2] = { DEMO_SHADER_DIR "/ssao.frag.spv",
                                       DEMO_SHADER_DIR "/composite.frag.spv" };
    VkRenderPass renderPasses[2] = { renderer.occlusionRenderPass, renderer.outputRenderPass };
    VkPipelineLayout layouts[2] = { renderer.scenePipelineLayout, renderer.compositePipelineLayout };
    VkPipeline* targets[2] = { &renderer.occlusionPipeline, &renderer.compositePipeline };
    for (int pass = 0; pass < 2; ++pass) {
        VkShaderModule vertexModule =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
        VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(fragmentShaders[pass]));
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
        fillCommonStates(vertexInput, inputAssembly, viewportState, rasterization, multisample,
                         depthStencil, dynamicState, false);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = blendAttachments;

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
        pipelineInfo.layout = layouts[pass];
        pipelineInfo.renderPass = renderPasses[pass];
        pipelineInfo.subpass = 0;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           targets[pass]));
        vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
        vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    }
}

void createRenderer(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    renderer = SsaoRenderer();

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

    std::vector<unsigned char> noisePixels;
    buildNoisePixels(noisePixels, 4);
    createTextureFromRgba(ctx, 4, 4, noisePixels.data(), false, renderer.noiseTexture, nullptr);

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
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.nearestSampler));

    createSceneRenderPass(ctx, renderer);
    createSingleColorRenderPass(ctx, OCCLUSION_FORMAT, false, renderer.occlusionRenderPass);
    createSingleColorRenderPass(ctx, ctx.swapchainFormat, true, renderer.outputRenderPass);
    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createPipelines(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SsaoFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(SsaoSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        frame.occlusionSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        frame.compositeSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SsaoFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.compositePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.occlusionPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scenePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.compositePipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.occlusionRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyTexture(ctx, renderer.noiseTexture);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, SsaoRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, SsaoRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SsaoSceneUniform& uniform, FrameStatistics& outStatistics)
{
    SsaoFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.sceneBuffer.mapped, &uniform, sizeof(SsaoSceneUniform));

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

    // 场景通道
    const double sceneStart = nowSeconds();
    VkClearValue sceneClearValues[3] = {};
    sceneClearValues[0].color.float32[3] = 1.0f;
    sceneClearValues[1].color.float32[2] = 1.0f;
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
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePipeline);
    vkCmdDraw(frame.commandBuffer, 4, renderer.instanceCount, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;

    // 颜色、深度与法线都要被后面的通道采样
    VkImageMemoryBarrier sceneRead[3] = {};
    const VkImage sceneImages[3] = { renderer.sceneColor.image, renderer.sceneNormal.image,
                                     renderer.sceneDepth.image };
    const VkImageAspectFlags sceneAspects[3] = { VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                                 VK_IMAGE_ASPECT_DEPTH_BIT };
    const VkImageLayout sceneOld[3] = { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    for (uint32_t i = 0; i < 3; ++i) {
        sceneRead[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sceneRead[i].oldLayout = sceneOld[i];
        sceneRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sceneRead[i].srcAccessMask = i == 2 ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                            : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sceneRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sceneRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneRead[i].image = sceneImages[i];
        sceneRead[i].subresourceRange.aspectMask = sceneAspects[i];
        sceneRead[i].subresourceRange.levelCount = 1;
        sceneRead[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, sceneRead);

    // 遮蔽通道
    const double occlusionStart = nowSeconds();
    VkRenderPassBeginInfo occlusionBegin = {};
    occlusionBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    occlusionBegin.renderPass = renderer.occlusionRenderPass;
    occlusionBegin.framebuffer = renderer.occlusionFramebuffer;
    occlusionBegin.renderArea.extent = ctx.swapchainExtent;
    occlusionBegin.clearValueCount = 0;

    vkCmdBeginRenderPass(frame.commandBuffer, &occlusionBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.occlusionSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.occlusionPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordOcclusionMilliseconds = (nowSeconds() - occlusionStart) * 1000.0;

    VkImageMemoryBarrier occlusionRead = {};
    occlusionRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    occlusionRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    occlusionRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    occlusionRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    occlusionRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    occlusionRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    occlusionRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    occlusionRead.image = renderer.occlusion.image;
    occlusionRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    occlusionRead.subresourceRange.levelCount = 1;
    occlusionRead.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &occlusionRead);

    // 合成通道
    const double compositeStart = nowSeconds();
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
                            renderer.compositePipelineLayout, 0, 1, &frame.compositeSet, 0, nullptr);
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
