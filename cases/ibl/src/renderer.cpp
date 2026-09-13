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
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

// 预计算的尺寸
enum { ENVIRONMENT_WIDTH = 256, ENVIRONMENT_HEIGHT = 128 };
enum { IRRADIANCE_WIDTH = 32, IRRADIANCE_HEIGHT = 16 };
enum { BRDF_SIZE = 128 };

// 预滤波链的基础尺寸，逐级减半
enum { PREFILTER_WIDTH = 128, PREFILTER_HEIGHT = 64 };

static float hash01(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0xffffffu) / static_cast<float>(0x1000000u);
}

// 金属度与粗糙度可调的方块阵：粗糙度从零铺到一，金属度交替
static void buildInstances(std::vector<glm::vec4>& data, uint32_t& outCount)
{
    data.clear();
    const uint32_t columns = 8;
    const uint32_t rows = 4;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            const uint32_t index = row * columns + column;
            const float x = -2.1f + static_cast<float>(column) * 0.6f;
            const float y = -0.9f + static_cast<float>(row) * 0.6f;
            const float z = -2.8f - 0.1f * static_cast<float>(row);
            const float size = 0.22f;
            const float roughness = static_cast<float>(column) / static_cast<float>(columns - 1);
            const float metalness = row < 2 ? 0.0f : 1.0f;
            const float hue = static_cast<float>(row * columns + column) / static_cast<float>(rows * columns);
            const glm::vec3 color = glm::vec3(0.5f + 0.5f * sin(hue * 6.2831853f),
                                              0.5f + 0.5f * sin(hue * 6.2831853f + 2.09f),
                                              0.5f + 0.5f * sin(hue * 6.2831853f + 4.18f));
            data.push_back(glm::vec4(x, y, z, size));
            data.push_back(glm::vec4(color, metalness));
            data.push_back(glm::vec4(roughness, hash01(index * 7u + 3u), 0.0f, 0.0f));
        }
    }
    outCount = rows * columns;
}

static void createColorRenderPass(const VulkanContext& ctx, VkFormat format, bool toPresent,
                                  VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

static void createSceneRenderPass(const VulkanContext& ctx, IblRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = HDR_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
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
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.sceneRenderPass));
}

static void createPrecomputeTargets(const VulkanContext& ctx, IblRenderer& renderer)
{
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ENVIRONMENT_WIDTH, ENVIRONMENT_HEIGHT, HDR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.environment);
    createAttachmentTexture(ctx, IRRADIANCE_WIDTH, IRRADIANCE_HEIGHT, HDR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.irradiance);
    createAttachmentTexture(ctx, BRDF_SIZE, BRDF_SIZE, HDR_FORMAT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                            renderer.brdfLut);
    // 预滤波链的每一级各一张贴图，尺寸逐级减半
    for (uint32_t level = 0; level < IBL_PREFILTER_LEVELS; ++level) {
        const uint32_t width = std::max(1u, static_cast<uint32_t>(PREFILTER_WIDTH) >> level);
        const uint32_t height = std::max(1u, static_cast<uint32_t>(PREFILTER_HEIGHT) >> level);
        createAttachmentTexture(ctx, width, height, HDR_FORMAT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                renderer.prefiltered[level]);
    }

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.precomputeRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.layers = 1;

    VkImageView views[3] = { renderer.environment.view, renderer.irradiance.view, renderer.brdfLut.view };
    const uint32_t widths[3] = { ENVIRONMENT_WIDTH, IRRADIANCE_WIDTH, BRDF_SIZE };
    const uint32_t heights[3] = { ENVIRONMENT_HEIGHT, IRRADIANCE_HEIGHT, BRDF_SIZE };
    for (uint32_t i = 0; i < 3; ++i) {
        framebufferInfo.pAttachments = &views[i];
        framebufferInfo.width = widths[i];
        framebufferInfo.height = heights[i];
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr,
                                     &renderer.precomputeFramebuffers[i]));
    }

    for (uint32_t level = 0; level < IBL_PREFILTER_LEVELS; ++level) {
        framebufferInfo.pAttachments = &renderer.prefiltered[level].view;
        framebufferInfo.width = renderer.prefiltered[level].width;
        framebufferInfo.height = renderer.prefiltered[level].height;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr,
                                     &renderer.prefilteredFramebuffers[level]));
    }
}

static void destroyPrecomputeTargets(const VulkanContext& ctx, IblRenderer& renderer)
{
    for (uint32_t level = 0; level < IBL_PREFILTER_LEVELS; ++level) {
        vkDestroyFramebuffer(ctx.device, renderer.prefilteredFramebuffers[level], nullptr);
        renderer.prefilteredFramebuffers[level] = VK_NULL_HANDLE;
        destroyTexture(ctx, renderer.prefiltered[level]);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.precomputeFramebuffers[i], nullptr);
        renderer.precomputeFramebuffers[i] = VK_NULL_HANDLE;
    }
    destroyTexture(ctx, renderer.brdfLut);
    destroyTexture(ctx, renderer.irradiance);
    destroyTexture(ctx, renderer.environment);
}

static void createSwapchainTargets(const VulkanContext& ctx, IblRenderer& renderer)
{
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, HDR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneColor);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.depthTexture);

    VkImageView sceneViews[2] = { renderer.sceneColor.view, renderer.depthTexture.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.sceneRenderPass;
    framebufferInfo.attachmentCount = 2;
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

static void destroySwapchainTargets(const VulkanContext& ctx, IblRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.sceneFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.depthTexture);
    destroyTexture(ctx, renderer.sceneColor);
}

static void createDescriptorLayouts(const VulkanContext& ctx, IblRenderer& renderer)
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
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.textureSetLayout));

    // 预滤波链的每一级各占一个绑定
    VkDescriptorSetLayoutBinding prefilterBindings[IBL_PREFILTER_LEVELS] = {};
    for (uint32_t i = 0; i < IBL_PREFILTER_LEVELS; ++i) {
        prefilterBindings[i].binding = i;
        prefilterBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        prefilterBindings[i].descriptorCount = 1;
        prefilterBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo prefilterLayoutInfo = {};
    prefilterLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    prefilterLayoutInfo.bindingCount = IBL_PREFILTER_LEVELS;
    prefilterLayoutInfo.pBindings = prefilterBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &prefilterLayoutInfo, nullptr,
                                         &renderer.prefilterSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 32;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 16;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 128;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeDescriptors(const VulkanContext& ctx, IblRenderer& renderer, uint32_t index)
{
    IblFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.precomputeSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);

    writeBufferDescriptor(ctx, frame.boxSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeBufferDescriptor(ctx, frame.boxSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.boxSet, 2, renderer.environment.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.boxSet, 3, renderer.irradiance.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.boxSet, 4, renderer.brdfLut.view, renderer.linearSampler);

    for (uint32_t level = 0; level < IBL_PREFILTER_LEVELS; ++level) {
        writeImageDescriptor(ctx, frame.prefilterSet, level, renderer.prefiltered[level].view,
                             renderer.linearSampler);
    }

    writeBufferDescriptor(ctx, frame.presentSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);
    writeImageDescriptor(ctx, frame.presentSet, 5, renderer.sceneColor.view, renderer.linearSampler);
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
                                           VkRenderPass renderPass, const char* vertexShader,
                                           const char* fragmentShader)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(vertexShader));
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

void createRenderer(const VulkanContext& ctx, IblRenderer& renderer)
{
    renderer = IblRenderer();

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

    // 立方体的索引：六个面各六个索引
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

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.linearSampler));

    createColorRenderPass(ctx, HDR_FORMAT, false, renderer.precomputeRenderPass);
    createSceneRenderPass(ctx, renderer);
    createColorRenderPass(ctx, ctx.swapchainFormat, true, renderer.outputRenderPass);

    createDescriptorLayouts(ctx, renderer);
    createPrecomputeTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.textureSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.presentPipelineLayout));

    // 方块管线要同时用到基础贴图与预滤波链两个描述符集
    const VkDescriptorSetLayout sceneLayouts[2] = { renderer.textureSetLayout,
                                                    renderer.prefilterSetLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = sceneLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.textureSetLayout;

    // 预滤波要用推入常量传粗糙度
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 4;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.precomputePipelineLayout));

    renderer.environmentPipeline =
        createFullscreenPipeline(ctx, renderer.precomputePipelineLayout, renderer.precomputeRenderPass,
                                 DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                 DEMO_SHADER_DIR "/environment.frag.spv");
    renderer.irradiancePipeline =
        createFullscreenPipeline(ctx, renderer.precomputePipelineLayout, renderer.precomputeRenderPass,
                                 DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                 DEMO_SHADER_DIR "/irradiance.frag.spv");
    renderer.prefilterPipeline =
        createFullscreenPipeline(ctx, renderer.precomputePipelineLayout, renderer.precomputeRenderPass,
                                 DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                 DEMO_SHADER_DIR "/prefilter.frag.spv");
    renderer.brdfPipeline =
        createFullscreenPipeline(ctx, renderer.precomputePipelineLayout, renderer.precomputeRenderPass,
                                 DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                 DEMO_SHADER_DIR "/brdf_lut.frag.spv");
    renderer.boxPipeline = createFullscreenPipeline(ctx, renderer.scenePipelineLayout,
                                                    renderer.sceneRenderPass,
                                                    DEMO_SHADER_DIR "/box.vert.spv",
                                                    DEMO_SHADER_DIR "/pbr.frag.spv");
    renderer.presentPipeline = createFullscreenPipeline(ctx, renderer.presentPipelineLayout,
                                                        renderer.outputRenderPass,
                                                        DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                        DEMO_SHADER_DIR "/present.frag.spv");

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        IblFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(IblSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.uniformBuffer);

        frame.precomputeSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.boxSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.prefilterSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.prefilterSetLayout);
        frame.presentSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, IblRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        IblFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.boxPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.brdfPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.prefilterPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.irradiancePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.environmentPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.precomputePipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.presentPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.prefilterSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.textureSetLayout, nullptr);

    destroyPrecomputeTargets(ctx, renderer);
    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.precomputeRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.indexBuffer);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, IblRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, IblRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const IblSceneUniform& uniform, FrameStatistics& outStatistics)
{
    IblFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(IblSceneUniform));

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

    // 预计算只在需要时跑：环境贴图、辐照度图、预滤波链、查找表
    const double precomputeStart = nowSeconds();
    if (input.options.recompute) {
        VkViewport viewport = {};
        viewport.maxDepth = 1.0f;
        VkRect2D scissor = {};

        const VkPipeline pipelines[4] = { renderer.environmentPipeline, renderer.irradiancePipeline,
                                          renderer.brdfPipeline, renderer.prefilterPipeline };
        for (uint32_t i = 0; i < 3; ++i) {
            const VkExtent2D extents[3] = { { ENVIRONMENT_WIDTH, ENVIRONMENT_HEIGHT },
                                            { IRRADIANCE_WIDTH, IRRADIANCE_HEIGHT },
                                            { BRDF_SIZE, BRDF_SIZE } };
            viewport.width = static_cast<float>(extents[i].width);
            viewport.height = static_cast<float>(extents[i].height);
            scissor.extent = extents[i];

            VkRenderPassBeginInfo begin = {};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = renderer.precomputeRenderPass;
            begin.framebuffer = renderer.precomputeFramebuffers[i];
            begin.renderArea.extent = extents[i];
            begin.clearValueCount = 0;

            vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    renderer.precomputePipelineLayout, 0, 1, &frame.precomputeSet, 0,
                                    nullptr);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            ++drawCallCount;
            vkCmdEndRenderPass(frame.commandBuffer);
            ++renderPassCount;
        }

        // 预滤波：每一级对应一个粗糙度
        for (uint32_t level = 0; level < IBL_PREFILTER_LEVELS; ++level) {
            const VkExtent2D extent = { std::max(1u, static_cast<uint32_t>(PREFILTER_WIDTH) >> level),
                                        std::max(1u, static_cast<uint32_t>(PREFILTER_HEIGHT) >> level) };
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            scissor.extent = extent;

            VkRenderPassBeginInfo begin = {};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = renderer.precomputeRenderPass;
            begin.framebuffer = renderer.prefilteredFramebuffers[level];
            begin.renderArea.extent = extent;
            begin.clearValueCount = 0;

            vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    renderer.precomputePipelineLayout, 0, 1, &frame.precomputeSet, 0,
                                    nullptr);
            const float roughness =
                static_cast<float>(level) / static_cast<float>(IBL_PREFILTER_LEVELS - 1);
            const float params[4] = { roughness, 0.0f, 0.0f, 0.0f };
            vkCmdPushConstants(frame.commandBuffer, renderer.precomputePipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              renderer.prefilterPipeline);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            ++drawCallCount;
            vkCmdEndRenderPass(frame.commandBuffer);
            ++renderPassCount;
        }
    }
    outStatistics.cpuRecordPrecomputeMilliseconds = (nowSeconds() - precomputeStart) * 1000.0;

    // 场景：方块的基于图像的光照
    const double sceneStart = nowSeconds();
    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    VkClearValue sceneClearValues[2] = {};
    sceneClearValues[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo sceneBegin = {};
    sceneBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    sceneBegin.renderPass = renderer.sceneRenderPass;
    sceneBegin.framebuffer = renderer.sceneFramebuffer;
    sceneBegin.renderArea.extent = ctx.swapchainExtent;
    sceneBegin.clearValueCount = 2;
    sceneBegin.pClearValues = sceneClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &sceneBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    const VkDescriptorSet boxSets[2] = { frame.boxSet, frame.prefilterSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 2, boxSets, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.boxPipeline);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(frame.commandBuffer, 36, renderer.instanceCount, 0, 0, 0);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);
    ++renderPassCount;
    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;

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
