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
static const VkFormat REVEAL_FORMAT = VK_FORMAT_R16_SFLOAT;

const char* transparencyModeName(uint32_t mode)
{
    switch (mode) {
        case TRANSPARENCY_NEAR_TO_FAR:
            return "near_to_far";
        case TRANSPARENCY_UNSORTED:
            return "unsorted";
        case TRANSPARENCY_WEIGHTED:
            return "weighted";
        case TRANSPARENCY_LINKED_LIST:
            return "linked_list";
        default:
            return "far_to_near";
    }
}

// 色相到 RGB，用来给每层一个可区分的颜色
static glm::vec3 hueToRgb(float hue)
{
    const float h = hue * 6.0f;
    const int sector = static_cast<int>(h) % 6;
    const float f = h - static_cast<float>(static_cast<int>(h));
    const float q = 1.0f - f;
    switch (sector) {
        case 0: return glm::vec3(1.0f, f, 0.0f);
        case 1: return glm::vec3(q, 1.0f, 0.0f);
        case 2: return glm::vec3(0.0f, 1.0f, f);
        case 3: return glm::vec3(0.0f, q, 1.0f);
        case 4: return glm::vec3(f, 0.0f, 1.0f);
        default: return glm::vec3(1.0f, 0.0f, q);
    }
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

static uint32_t floatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// 把层数换算成色片：深度从小到大排列，颜色按色相铺开，位置带一点抖动让边缘处层数递减
static void buildLayerSet(std::vector<TransparencyLayer>& base, uint32_t count)
{
    base.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float depth = 0.15f + 0.8f * t;
        const glm::vec3 color = hueToRgb(t);
        const float x = (hash01(i * 2u + 1u) - 0.5f) * 0.24f;
        const float y = (hash01(i * 2u + 2u) - 0.5f) * 0.24f;
        base.push_back({ glm::vec4(x, y, 1.1f, 0.35f), glm::vec4(color, depth) });
    }
}

// 三种顺序各占一段：由远到近、由近到远、乱序
static void buildLayers(std::vector<TransparencyLayer>& layers, uint32_t count)
{
    std::vector<TransparencyLayer> base;
    buildLayerSet(base, count);

    std::vector<TransparencyLayer> farToNear = base;
    std::vector<TransparencyLayer> nearToFar = base;
    std::vector<TransparencyLayer> unsorted = base;
    std::stable_sort(farToNear.begin(), farToNear.end(),
                     [](const TransparencyLayer& a, const TransparencyLayer& b) { return a.color.w > b.color.w; });
    std::stable_sort(nearToFar.begin(), nearToFar.end(),
                     [](const TransparencyLayer& a, const TransparencyLayer& b) { return a.color.w < b.color.w; });
    std::stable_sort(unsorted.begin(), unsorted.end(), [](const TransparencyLayer& a, const TransparencyLayer& b) {
        return hash01(floatBits(a.color.w)) < hash01(floatBits(b.color.w));
    });

    layers.clear();
    layers.insert(layers.end(), farToNear.begin(), farToNear.end());
    layers.insert(layers.end(), nearToFar.begin(), nearToFar.end());
    layers.insert(layers.end(), unsorted.begin(), unsorted.end());
}

// 源混合通道：一张高动态范围颜色附件，清除为背景色
static void createSourceRenderPass(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = HDR_FORMAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.sourceRenderPass));
}

// 加权混合通道：累积颜色与累积权重一张附件，露出度一张附件
static void createWeightedRenderPass(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = HDR_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[1].format = REVEAL_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.weightedRenderPass));
}

// 输出通道：写交换链图像，界面也记录在这个通道里
static void createOutputRenderPass(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = ctx.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.outputRenderPass));
}

static void createOffscreenTargets(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    const uint32_t width = ctx.swapchainExtent.width;
    const uint32_t height = ctx.swapchainExtent.height;

    createAttachmentTexture(ctx, width, height, HDR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.hdrColor);
    createAttachmentTexture(ctx, width, height, HDR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.accumColor);
    createAttachmentTexture(ctx, width, height, REVEAL_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.revealage);

    VkImageView sourceViews[1] = { renderer.hdrColor.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.sourceRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = sourceViews;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.sourceFramebuffer));

    VkImageView weightedViews[2] = { renderer.accumColor.view, renderer.revealage.view };
    framebufferInfo.renderPass = renderer.weightedRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = weightedViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.weightedFramebuffer));

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

    // 每像素的叠加次数缓冲
    createBuffer(ctx, sizeof(uint32_t) * width * height,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.layerCountBuffer);
    // 每像素的链表头
    createBuffer(ctx, sizeof(uint32_t) * width * height,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.headBuffer);
}

static void destroyOffscreenTargets(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    destroyBuffer(ctx, renderer.headBuffer);
    destroyBuffer(ctx, renderer.layerCountBuffer);
    vkDestroyFramebuffer(ctx.device, renderer.weightedFramebuffer, nullptr);
    renderer.weightedFramebuffer = VK_NULL_HANDLE;
    vkDestroyFramebuffer(ctx.device, renderer.sourceFramebuffer, nullptr);
    renderer.sourceFramebuffer = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.revealage);
    destroyTexture(ctx, renderer.accumColor);
    destroyTexture(ctx, renderer.hdrColor);
}

static void createDescriptorLayouts(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkDescriptorSetLayoutBinding sceneBindings[7] = {};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[1].descriptorCount = 1;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    for (uint32_t i = 2; i <= 6; ++i) {
        sceneBindings[i].binding = i;
        sceneBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sceneBindings[i].descriptorCount = 1;
        sceneBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo sceneLayoutInfo = {};
    sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sceneLayoutInfo.bindingCount = 7;
    sceneLayoutInfo.pBindings = sceneBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &sceneLayoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorSetLayoutBinding outputBindings[8] = {};
    outputBindings[0].binding = 0;
    outputBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    outputBindings[0].descriptorCount = 1;
    outputBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i <= 3; ++i) {
        outputBindings[i].binding = i;
        outputBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        outputBindings[i].descriptorCount = 1;
        outputBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (uint32_t i = 4; i <= 7; ++i) {
        outputBindings[i].binding = i;
        outputBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        outputBindings[i].descriptorCount = 1;
        outputBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo outputLayoutInfo = {};
    outputLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    outputLayoutInfo.bindingCount = 8;
    outputLayoutInfo.pBindings = outputBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &outputLayoutInfo, nullptr, &renderer.outputSetLayout));
}

static void createDescriptorPool(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 64;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 16;

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

static void fillCommonStates(VkPipelineVertexInputStateCreateInfo& vertexInput,
                             VkPipelineInputAssemblyStateCreateInfo& inputAssembly,
                             VkPipelineViewportStateCreateInfo& viewportState,
                             VkPipelineRasterizationStateCreateInfo& rasterization,
                             VkPipelineMultisampleStateCreateInfo& multisample,
                             VkPipelineDepthStencilStateCreateInfo& depthStencil,
                             VkPipelineDynamicStateCreateInfo& dynamicState)
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

    static VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;
}

// 源混合与逐像素链表共用同一个渲染通道，只有混合状态与片元着色器不同
static VkPipeline createScenePipeline(const VulkanContext& ctx, TransparencyRenderer& renderer,
                                      VkRenderPass renderPass, VkShaderModule vertexModule,
                                      VkShaderModule fragmentModule, bool weighted, bool writeColor)
{
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
    fillCommonStates(vertexInput, inputAssembly, viewportState, rasterization, multisample, depthStencil,
                     dynamicState);

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    const uint32_t attachmentCount = weighted ? 2u : 1u;
    blendAttachments[0].colorWriteMask = writeColor
                                             ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
                                             : 0;
    if (weighted) {
        // 累积颜色与累积权重都用加法，露出度按源颜色衰减
        blendAttachments[0].blendEnable = VK_TRUE;
        blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;

        blendAttachments[1].blendEnable = VK_TRUE;
        blendAttachments[1].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachments[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        blendAttachments[1].colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachments[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachments[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[1].alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    } else if (writeColor) {
        // 源混合：颜色按源 alpha 混合，深度不参与
        blendAttachments[0].blendEnable = VK_TRUE;
        blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = attachmentCount;
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
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    return pipeline;
}

static void createScenePipelines(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));

    VkShaderModule vertexModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/quad.vert.spv"));
    VkShaderModule sourceModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/quad.frag.spv"));
    VkShaderModule weightedModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/weighted.frag.spv"));
    VkShaderModule listModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/list.frag.spv"));

    renderer.sourcePipeline =
        createScenePipeline(ctx, renderer, renderer.sourceRenderPass, vertexModule, sourceModule, false, true);
    renderer.listPipeline =
        createScenePipeline(ctx, renderer, renderer.sourceRenderPass, vertexModule, listModule, false, false);
    renderer.weightedPipeline =
        createScenePipeline(ctx, renderer, renderer.weightedRenderPass, vertexModule, weightedModule, true, true);

    vkDestroyShaderModule(ctx.device, listModule, nullptr);
    vkDestroyShaderModule(ctx.device, weightedModule, nullptr);
    vkDestroyShaderModule(ctx.device, sourceModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

static VkPipeline createOutputPipeline(const VulkanContext& ctx, TransparencyRenderer& renderer,
                                       const char* fragmentShader)
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
    fillCommonStates(vertexInput, inputAssembly, viewportState, rasterization, multisample, depthStencil,
                     dynamicState);
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

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
    pipelineInfo.layout = renderer.outputPipelineLayout;
    pipelineInfo.renderPass = renderer.outputRenderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    return pipeline;
}

static void createOutputPipelines(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.outputSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.outputPipelineLayout));

    renderer.compositePipeline = createOutputPipeline(ctx, renderer, DEMO_SHADER_DIR "/composite.frag.spv");
    renderer.weightedResolvePipeline =
        createOutputPipeline(ctx, renderer, DEMO_SHADER_DIR "/weighted_resolve.frag.spv");
    renderer.listResolvePipeline =
        createOutputPipeline(ctx, renderer, DEMO_SHADER_DIR "/list_resolve.frag.spv");
}

static void writeSceneDescriptors(const VulkanContext& ctx, TransparencyRenderer& renderer, uint32_t index)
{
    TransparencyFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.layerBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.layerCountBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.headBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.nodeColorBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.nodeMetaBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.nodeCounterBuffer);
}

static void writeOutputDescriptors(const VulkanContext& ctx, TransparencyRenderer& renderer, uint32_t index)
{
    TransparencyFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.outputSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeImageDescriptor(ctx, frame.outputSet, 1, renderer.hdrColor.view, renderer.sampler);
    writeImageDescriptor(ctx, frame.outputSet, 2, renderer.accumColor.view, renderer.sampler);
    writeImageDescriptor(ctx, frame.outputSet, 3, renderer.revealage.view, renderer.sampler);
    writeBufferDescriptor(ctx, frame.outputSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.headBuffer);
    writeBufferDescriptor(ctx, frame.outputSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.nodeColorBuffer);
    writeBufferDescriptor(ctx, frame.outputSet, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.nodeMetaBuffer);
    writeBufferDescriptor(ctx, frame.outputSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.layerCountBuffer);
}

void createRenderer(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    renderer = TransparencyRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    // 层数与顺序每帧都可能变化，缓冲放在主机可见内存里逐帧写入
    createBuffer(ctx, sizeof(glm::vec4) * 2 * TRANSPARENCY_MAX_LAYERS * 3,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.layerBuffer);
    createBuffer(ctx, sizeof(glm::vec4) * TRANSPARENCY_NODE_CAPACITY, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.nodeColorBuffer);
    createBuffer(ctx, sizeof(uint32_t) * 2 * TRANSPARENCY_NODE_CAPACITY,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 renderer.nodeMetaBuffer);
    createBuffer(ctx, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.nodeCounterBuffer);

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.sampler));

    createSourceRenderPass(ctx, renderer);
    createWeightedRenderPass(ctx, renderer);
    createOutputRenderPass(ctx, renderer);
    createOffscreenTargets(ctx, renderer);

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createScenePipelines(ctx, renderer);
    createOutputPipelines(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        TransparencyFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(TransparencySceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        frame.outputSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.outputSetLayout);
        writeSceneDescriptors(ctx, renderer, static_cast<uint32_t>(i));
        writeOutputDescriptors(ctx, renderer, static_cast<uint32_t>(i));

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

void destroyRenderer(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        TransparencyFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.listResolvePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.weightedResolvePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.compositePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.outputPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.listPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.weightedPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.sourcePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.outputSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroyOffscreenTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.weightedRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sourceRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.sampler, nullptr);
    destroyBuffer(ctx, renderer.nodeCounterBuffer);
    destroyBuffer(ctx, renderer.nodeMetaBuffer);
    destroyBuffer(ctx, renderer.nodeColorBuffer);
    destroyBuffer(ctx, renderer.layerBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, TransparencyRenderer& renderer)
{
    destroyOffscreenTargets(ctx, renderer);
    createOffscreenTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeOutputDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, TransparencyRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const TransparencySceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    TransparencyFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    // 上一帧写入的链表节点数
    uint32_t nodeCount = 0;
    std::memcpy(&nodeCount, renderer.nodeCounterBuffer.mapped, sizeof(uint32_t));
    outStatistics.nodeCount = std::min(nodeCount, static_cast<uint32_t>(TRANSPARENCY_NODE_CAPACITY));
    outStatistics.nodeOverflow = nodeCount > TRANSPARENCY_NODE_CAPACITY
                                     ? nodeCount - static_cast<uint32_t>(TRANSPARENCY_NODE_CAPACITY)
                                     : 0u;
    outStatistics.listBytes = static_cast<uint64_t>(TRANSPARENCY_NODE_CAPACITY) *
                              (sizeof(glm::vec4) + sizeof(glm::uvec2));

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

    const uint32_t layerCount = std::max(1u, std::min(input.options.layerCount, 12u));
    std::vector<TransparencyLayer> layers;
    buildLayers(layers, layerCount);
    std::memcpy(renderer.layerBuffer.mapped, layers.data(), sizeof(TransparencyLayer) * layers.size());

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(TransparencySceneUniform));

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

    const uint32_t mode = input.options.mode;
    const bool listMode = mode == TRANSPARENCY_LINKED_LIST;
    const bool weightedMode = mode == TRANSPARENCY_WEIGHTED;
    const bool heatmap = input.options.heatmap;
    uint32_t drawCallCount = 0;

    const double geometryStart = nowSeconds();

    // 逐像素链表与热力图都需要把缓冲先清干净
    if (listMode) {
        const uint32_t empty = 0xFFFFFFFFu;
        vkCmdFillBuffer(frame.commandBuffer, renderer.headBuffer.buffer, 0, VK_WHOLE_SIZE, empty);
        vkCmdFillBuffer(frame.commandBuffer, renderer.nodeCounterBuffer.buffer, 0, sizeof(uint32_t), 0u);

        VkMemoryBarrier clearBarrier = {};
        clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &clearBarrier, 0, nullptr, 0,
                             nullptr);
    }
    if (heatmap) {
        vkCmdFillBuffer(frame.commandBuffer, renderer.layerCountBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);

        VkMemoryBarrier clearBarrier = {};
        clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &clearBarrier, 0, nullptr, 0,
                             nullptr);
    }

    VkClearValue clearValues[2] = {};
    clearValues[0].color.float32[0] = sceneUniform.colorParams.x;
    clearValues[0].color.float32[1] = sceneUniform.colorParams.y;
    clearValues[0].color.float32[2] = sceneUniform.colorParams.z;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].color.float32[0] = 1.0f;  // 露出度初始为一

    VkRenderPassBeginInfo geometryBegin = {};
    geometryBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geometryBegin.renderPass = weightedMode ? renderer.weightedRenderPass : renderer.sourceRenderPass;
    geometryBegin.framebuffer = weightedMode ? renderer.weightedFramebuffer : renderer.sourceFramebuffer;
    geometryBegin.renderArea.extent = ctx.swapchainExtent;
    geometryBegin.clearValueCount = weightedMode ? 2 : 1;
    geometryBegin.pClearValues = clearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &geometryBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);

    VkPipeline pipeline = renderer.sourcePipeline;
    uint32_t firstInstance = 0;
    if (weightedMode) {
        pipeline = renderer.weightedPipeline;
    } else if (listMode) {
        pipeline = renderer.listPipeline;
    } else if (mode == TRANSPARENCY_NEAR_TO_FAR) {
        firstInstance = layerCount;
    } else if (mode == TRANSPARENCY_UNSORTED) {
        firstInstance = 2 * layerCount;
    }

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(frame.commandBuffer, 4, layerCount, 0, firstInstance);
    ++drawCallCount;
    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordGeometryPassMilliseconds = (nowSeconds() - geometryStart) * 1000.0;

    const double resolveStart = nowSeconds();

    // 几何通道的结果交给输出通道采样
    VkImageMemoryBarrier imageBarriers[2] = {};
    const VkImage images[2] = { weightedMode ? renderer.accumColor.image : renderer.hdrColor.image,
                                renderer.revealage.image };
    const uint32_t imageCount = weightedMode ? 2u : 1u;
    for (uint32_t i = 0; i < imageCount; ++i) {
        imageBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarriers[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageBarriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageBarriers[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imageBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        imageBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarriers[i].image = images[i];
        imageBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarriers[i].subresourceRange.levelCount = 1;
        imageBarriers[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, imageCount,
                         imageBarriers);

    // 链表与叠加次数缓冲由片元着色器写入，输出通道要读它们
    if (listMode || heatmap) {
        VkMemoryBarrier bufferBarrier = {};
        bufferBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        bufferBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &bufferBarrier, 0, nullptr, 0,
                             nullptr);
    }

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
                            renderer.outputPipelineLayout, 0, 1, &frame.outputSet, 0, nullptr);

    VkPipeline outputPipeline = renderer.compositePipeline;
    if (weightedMode) {
        outputPipeline = renderer.weightedResolvePipeline;
    } else if (listMode) {
        outputPipeline = renderer.listResolvePipeline;
    }
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    vkCmdEndRenderPass(frame.commandBuffer);
    outStatistics.cpuRecordResolveMilliseconds = (nowSeconds() - resolveStart) * 1000.0;

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
