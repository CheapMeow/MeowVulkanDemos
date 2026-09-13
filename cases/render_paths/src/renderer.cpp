#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
enum { MIN_TILE_SIZE = 8 };

const char* renderPathName(uint32_t path)
{
    if (path == RENDER_PATH_DEFERRED) {
        return "deferred";
    }
    if (path == RENDER_PATH_TILED) {
        return "tiled";
    }
    return "forward";
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

// 一堆绕竖直轴偏转的板，光源在它们之间穿行
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

// 光源在场景前方的一块体积里缓慢移动
static void buildLights(std::vector<RenderPathLight>& lights, uint32_t count, float time)
{
    lights.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const float fi = static_cast<float>(i);
        const float phase = fi * 1.7f + time * 0.4f;
        const float x = -1.8f + 3.6f * hash01(i * 13u + 1u) + 0.25f * sin(phase);
        const float y = -1.2f + 2.4f * hash01(i * 17u + 5u) + 0.2f * cos(phase * 1.3f);
        const float z = -1.6f - 2.4f * hash01(i * 19u + 7u);
        const float radius = 0.6f + 0.9f * hash01(i * 23u + 11u);
        const float hue = hash01(i * 29u + 13u);
        const glm::vec3 color =
            glm::vec3(0.5f + 0.5f * sin(hue * 6.2831853f), 0.5f + 0.5f * sin(hue * 6.2831853f + 2.09f),
                      0.5f + 0.5f * sin(hue * 6.2831853f + 4.18f));
        lights.push_back({ glm::vec4(x, y, z, radius), glm::vec4(color, 1.1f) });
    }
}

static void fillColorAttachment(VkAttachmentDescription& attachment, VkFormat format,
                                VkImageLayout finalLayout, bool preserve)
{
    attachment = VkAttachmentDescription();
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = preserve ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout =
        preserve ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = finalLayout;
}

// 前向通道：一张颜色附件加深度
static void createForwardRenderPass(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    VkAttachmentDescription attachments[2];
    fillColorAttachment(attachments[0], COLOR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    fillColorAttachment(attachments[1], DEPTH_FORMAT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        false);

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
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.forwardRenderPass));
}

// 几何通道：三张颜色附件加深度
static void createGbufferRenderPass(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    VkAttachmentDescription attachments[4];
    fillColorAttachment(attachments[0], COLOR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    fillColorAttachment(attachments[1], COLOR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    fillColorAttachment(attachments[2], COLOR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    fillColorAttachment(attachments[3], DEPTH_FORMAT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        false);

    VkAttachmentReference colorRefs[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentReference depthRef = {};
    depthRef.attachment = 3;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 3;
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
    renderPassInfo.attachmentCount = 4;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.gbufferRenderPass));
}

static void createSingleColorRenderPass(const VulkanContext& ctx, VkFormat format,
                                        VkImageLayout finalLayout, VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachment;
    fillColorAttachment(attachment, format, finalLayout, false);
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

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

static void createSwapchainTargets(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.forwardColor);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.gbufferAlbedo);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.gbufferNormal);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.gbufferPosition);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT, renderer.depthTexture);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, COLOR_FORMAT, usage,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.litTexture);

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;

    VkImageView forwardViews[2] = { renderer.forwardColor.view, renderer.depthTexture.view };
    framebufferInfo.renderPass = renderer.forwardRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = forwardViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.forwardFramebuffer));

    VkImageView gbufferViews[4] = { renderer.gbufferAlbedo.view, renderer.gbufferNormal.view,
                                    renderer.gbufferPosition.view, renderer.depthTexture.view };
    framebufferInfo.renderPass = renderer.gbufferRenderPass;
    framebufferInfo.attachmentCount = 4;
    framebufferInfo.pAttachments = gbufferViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.gbufferFramebuffer));

    VkImageView litViews[1] = { renderer.litTexture.view };
    framebufferInfo.renderPass = renderer.litRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = litViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.litFramebuffer));

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

static void destroySwapchainTargets(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    vkDestroyFramebuffer(ctx.device, renderer.litFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.gbufferFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.forwardFramebuffer, nullptr);
    renderer.litFramebuffer = VK_NULL_HANDLE;
    renderer.gbufferFramebuffer = VK_NULL_HANDLE;
    renderer.forwardFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.litTexture);
    destroyTexture(ctx, renderer.depthTexture);
    destroyTexture(ctx, renderer.gbufferPosition);
    destroyTexture(ctx, renderer.gbufferNormal);
    destroyTexture(ctx, renderer.gbufferAlbedo);
    destroyTexture(ctx, renderer.forwardColor);
}

static void createDescriptorLayouts(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[8] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 5; i <= 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorSetLayoutBinding tileBindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        tileBindings[i].binding = i;
        tileBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tileBindings[i].descriptorCount = 1;
        tileBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo tileLayoutInfo = {};
    tileLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tileLayoutInfo.bindingCount = 2;
    tileLayoutInfo.pBindings = tileBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &tileLayoutInfo, nullptr, &renderer.tileSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 32;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeDescriptors(const VulkanContext& ctx, RenderPathRenderer& renderer, uint32_t index)
{
    RenderPathFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.uniformBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.lightBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.tileBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.tileLightBuffer);
    writeImageDescriptor(ctx, frame.sceneSet, 5, renderer.gbufferAlbedo.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 6, renderer.gbufferNormal.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 7, renderer.gbufferPosition.view, renderer.nearestSampler);

    writeBufferDescriptor(ctx, frame.tileSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.tileBuffer);
    writeBufferDescriptor(ctx, frame.tileSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.tileLightBuffer);

    // 两条路径的输出源不同，各准备一个描述符集，避免在录制命令时改写已绑定的集
    writeBufferDescriptor(ctx, frame.presentForwardSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);
    writeImageDescriptor(ctx, frame.presentForwardSet, 5, renderer.forwardColor.view,
                         renderer.linearSampler);
    writeBufferDescriptor(ctx, frame.presentDeferredSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          frame.uniformBuffer);
    writeImageDescriptor(ctx, frame.presentDeferredSet, 5, renderer.litTexture.view,
                         renderer.linearSampler);
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

static VkPipeline createGraphicsPipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                         VkRenderPass renderPass, const char* vertexShader,
                                         const char* fragmentShader, uint32_t attachmentCount,
                                         bool depthTest)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(vertexShader));
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
                     dynamicState, depthTest);
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineColorBlendAttachmentState blendAttachments[3] = {};
    for (uint32_t i = 0; i < attachmentCount; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    return pipeline;
}

void createRenderer(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    renderer = RenderPathRenderer();

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

    createBuffer(ctx, sizeof(RenderPathLight) * MAX_LIGHT_COUNT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.lightBuffer);

    // 分块缓冲按最小分块尺寸一次分配，运行时只用到前面的部分
    const uint32_t maxTiles = ((ctx.swapchainExtent.width + MIN_TILE_SIZE - 1) / MIN_TILE_SIZE) *
                              ((ctx.swapchainExtent.height + MIN_TILE_SIZE - 1) / MIN_TILE_SIZE);
    renderer.tileCapacity = maxTiles;
    createBuffer(ctx, sizeof(uint32_t) * 4 * maxTiles,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.tileBuffer);
    createBuffer(ctx, sizeof(uint32_t) * 64 * maxTiles,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.tileLightBuffer);

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

    createForwardRenderPass(ctx, renderer);
    createGbufferRenderPass(ctx, renderer);
    createSingleColorRenderPass(ctx, COLOR_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                renderer.litRenderPass);
    createSingleColorRenderPass(ctx, ctx.swapchainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                renderer.outputRenderPass);

    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.lightingPipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.presentPipelineLayout));

    const VkDescriptorSetLayout tileLayouts[2] = { renderer.sceneSetLayout, renderer.tileSetLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = tileLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.tilePipelineLayout));

    renderer.forwardPipeline = createGraphicsPipeline(ctx, renderer.scenePipelineLayout,
                                                      renderer.forwardRenderPass,
                                                      DEMO_SHADER_DIR "/scene.vert.spv",
                                                      DEMO_SHADER_DIR "/forward.frag.spv", 1, true);
    renderer.gbufferPipeline = createGraphicsPipeline(ctx, renderer.scenePipelineLayout,
                                                      renderer.gbufferRenderPass,
                                                      DEMO_SHADER_DIR "/scene.vert.spv",
                                                      DEMO_SHADER_DIR "/gbuffer.frag.spv", 3, true);
    renderer.deferredLightingPipeline =
        createGraphicsPipeline(ctx, renderer.lightingPipelineLayout, renderer.litRenderPass,
                               DEMO_SHADER_DIR "/fullscreen.vert.spv",
                               DEMO_SHADER_DIR "/deferred_lighting.frag.spv", 1, false);
    renderer.presentPipeline = createGraphicsPipeline(ctx, renderer.presentPipelineLayout,
                                                      renderer.outputRenderPass,
                                                      DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                      DEMO_SHADER_DIR "/present.frag.spv", 1, false);

    {
        VkShaderModule module =
            loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/tile.comp.spv"));
        VkPipelineShaderStageCreateInfo stage = makeShaderStage(VK_SHADER_STAGE_COMPUTE_BIT, module);
        VkComputePipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = renderer.tilePipelineLayout;
        VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &renderer.tilePipeline));
        vkDestroyShaderModule(ctx.device, module, nullptr);
    }

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        RenderPathFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(RenderPathSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.uniformBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        frame.tileSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.tileSetLayout);
        frame.presentForwardSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        frame.presentDeferredSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        RenderPathFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.tilePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.deferredLightingPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.gbufferPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.forwardPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.tilePipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.presentPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.lightingPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.tileSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.litRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.gbufferRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.forwardRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.tileLightBuffer);
    destroyBuffer(ctx, renderer.tileBuffer);
    destroyBuffer(ctx, renderer.lightBuffer);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void updateLights(RenderPathRenderer& renderer, uint32_t lightCount, float time)
{
    std::vector<RenderPathLight> lights;
    buildLights(lights, std::min(lightCount, static_cast<uint32_t>(MAX_LIGHT_COUNT)), time);
    if (!lights.empty()) {
        std::memcpy(renderer.lightBuffer.mapped, lights.data(), sizeof(RenderPathLight) * lights.size());
    }
}

void recreateSwapchainTargets(const VulkanContext& ctx, RenderPathRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, RenderPathRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const RenderPathSceneUniform& uniform,
               FrameStatistics& outStatistics)
{
    RenderPathFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(RenderPathSceneUniform));

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

    const uint32_t path = input.options.path;
    const uint32_t lightCount = std::min(input.options.lightCount, static_cast<uint32_t>(MAX_LIGHT_COUNT));
    const uint32_t tileSize = std::max(input.options.tileSize, static_cast<uint32_t>(MIN_TILE_SIZE));
    const uint32_t maxLightsPerTile = std::min(std::max(input.options.maxLightsPerTile, 1u), 64u);

    const uint32_t tilesPerRow = (ctx.swapchainExtent.width + tileSize - 1) / tileSize;
    const uint32_t tilesPerColumn = (ctx.swapchainExtent.height + tileSize - 1) / tileSize;
    const uint32_t tileCount = tilesPerRow * tilesPerColumn;
    outStatistics.tileCount = tileCount;

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    uint32_t drawCallCount = 0;

    // 分块前向：先把每块的光源列表清空再重建
    const double tileStart = nowSeconds();
    if (path == RENDER_PATH_TILED) {
        vkCmdFillBuffer(frame.commandBuffer, renderer.tileBuffer.buffer, 0,
                        sizeof(uint32_t) * 4 * tileCount, 0u);
        vkCmdFillBuffer(frame.commandBuffer, renderer.tileLightBuffer.buffer, 0,
                        sizeof(uint32_t) * maxLightsPerTile * tileCount, 0u);

        VkBufferMemoryBarrier barriers[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].buffer = i == 0 ? renderer.tileBuffer.buffer : renderer.tileLightBuffer.buffer;
            barriers[i].offset = 0;
            barriers[i].size = i == 0 ? sizeof(uint32_t) * 4 * tileCount
                                      : sizeof(uint32_t) * maxLightsPerTile * tileCount;
        }
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2, barriers, 0, nullptr);

        const VkDescriptorSet tileSets[2] = { frame.sceneSet, frame.tileSet };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.tilePipelineLayout, 0, 2, tileSets, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.tilePipeline);
        vkCmdDispatch(frame.commandBuffer, (tilesPerRow + 7) / 8, (tilesPerColumn + 7) / 8, 1);

        VkBufferMemoryBarrier tileWrite = {};
        tileWrite.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        tileWrite.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        tileWrite.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        tileWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tileWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tileWrite.buffer = renderer.tileBuffer.buffer;
        tileWrite.offset = 0;
        tileWrite.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &tileWrite, 0,
                             nullptr);
    }
    outStatistics.cpuRecordTileMilliseconds = (nowSeconds() - tileStart) * 1000.0;

    const double geometryStart = nowSeconds();
    if (path == RENDER_PATH_DEFERRED) {
        VkClearValue clearValues[4] = {};
        clearValues[1].color.float32[2] = 1.0f;
        clearValues[1].color.float32[3] = 1.0f;
        clearValues[3].depthStencil.depth = 1.0f;

        VkRenderPassBeginInfo gbufferBegin = {};
        gbufferBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        gbufferBegin.renderPass = renderer.gbufferRenderPass;
        gbufferBegin.framebuffer = renderer.gbufferFramebuffer;
        gbufferBegin.renderArea.extent = ctx.swapchainExtent;
        gbufferBegin.clearValueCount = 4;
        gbufferBegin.pClearValues = clearValues;

        vkCmdBeginRenderPass(frame.commandBuffer, &gbufferBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gbufferPipeline);
        vkCmdDraw(frame.commandBuffer, 4, renderer.instanceCount, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
    } else {
        VkClearValue clearValues[2] = {};
        clearValues[0].color.float32[3] = 1.0f;
        clearValues[1].depthStencil.depth = 1.0f;

        VkRenderPassBeginInfo forwardBegin = {};
        forwardBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        forwardBegin.renderPass = renderer.forwardRenderPass;
        forwardBegin.framebuffer = renderer.forwardFramebuffer;
        forwardBegin.renderArea.extent = ctx.swapchainExtent;
        forwardBegin.clearValueCount = 2;
        forwardBegin.pClearValues = clearValues;

        vkCmdBeginRenderPass(frame.commandBuffer, &forwardBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.scenePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.forwardPipeline);
        vkCmdDraw(frame.commandBuffer, 4, renderer.instanceCount, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
    }
    outStatistics.cpuRecordGeometryMilliseconds = (nowSeconds() - geometryStart) * 1000.0;

    const double lightingStart = nowSeconds();
    if (path == RENDER_PATH_DEFERRED) {
        VkRenderPassBeginInfo litBegin = {};
        litBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        litBegin.renderPass = renderer.litRenderPass;
        litBegin.framebuffer = renderer.litFramebuffer;
        litBegin.renderArea.extent = ctx.swapchainExtent;
        litBegin.clearValueCount = 0;

        vkCmdBeginRenderPass(frame.commandBuffer, &litBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.lightingPipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          renderer.deferredLightingPipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
    }
    outStatistics.cpuRecordLightingMilliseconds = (nowSeconds() - lightingStart) * 1000.0;

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

    // 延迟路径的合成读光照结果，前向与分块前向直接读前向颜色
    const VkDescriptorSet presentSet =
        path == RENDER_PATH_DEFERRED ? frame.presentDeferredSet : frame.presentForwardSet;
    const VkPipelineLayout presentLayout = renderer.presentPipelineLayout;
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, presentLayout, 0, 1,
                            &presentSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.presentPipeline);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    ++drawCallCount;

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
