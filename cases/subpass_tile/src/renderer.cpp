#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat GBUFFER_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

const char* subpassPathName(uint32_t path)
{
    return path == SUBPASS_PATH_SUBPASS ? "subpass" : "two_passes";
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

// 三排相互遮挡的板，几何缓冲上有明显的深度与法线台阶
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

// 一个颜色附件，可以指定是否把内容写到主存
static void fillColorAttachment(VkAttachmentDescription& attachment, VkFormat format,
                                VkAttachmentStoreOp storeOp, VkImageLayout finalLayout)
{
    attachment = VkAttachmentDescription();
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = storeOp;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = finalLayout;
}

// 几何通道：两个颜色附件加深度，内容保留在主存里
static void createGeometryRenderPass(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    VkAttachmentDescription attachments[3];
    fillColorAttachment(attachments[0], GBUFFER_FORMAT, VK_ATTACHMENT_STORE_OP_STORE,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fillColorAttachment(attachments[1], GBUFFER_FORMAT, VK_ATTACHMENT_STORE_OP_STORE,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fillColorAttachment(attachments[2], DEPTH_FORMAT, VK_ATTACHMENT_STORE_OP_STORE,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

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
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.geometryRenderPass));
}

// 单个颜色附件的通道，给光照、乒乓与输出共用
static void createSingleColorRenderPass(const VulkanContext& ctx, VkFormat format,
                                        VkImageLayout finalLayout, VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachment;
    fillColorAttachment(attachment, format, VK_ATTACHMENT_STORE_OP_STORE, finalLayout);
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

// 子通道路径：几何写进附件，光照子通道把它们当输入附件读，全程不落到主存
static void createSubpassRenderPass(const VulkanContext& ctx, SubpassRenderer& renderer,
                                    VkAttachmentStoreOp geometryStoreOp, VkRenderPass& outRenderPass)
{
    VkAttachmentDescription attachments[4];
    fillColorAttachment(attachments[0], GBUFFER_FORMAT, geometryStoreOp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fillColorAttachment(attachments[1], GBUFFER_FORMAT, geometryStoreOp, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fillColorAttachment(attachments[2], DEPTH_FORMAT, geometryStoreOp, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    fillColorAttachment(attachments[3], GBUFFER_FORMAT, VK_ATTACHMENT_STORE_OP_STORE,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

    VkAttachmentReference geometryColorRefs[2] = {};
    geometryColorRefs[0].attachment = 0;
    geometryColorRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    geometryColorRefs[1].attachment = 1;
    geometryColorRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference geometryDepthRef = {};
    geometryDepthRef.attachment = 2;
    geometryDepthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference inputRefs[2] = {};
    inputRefs[0].attachment = 0;
    inputRefs[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputRefs[1].attachment = 1;
    inputRefs[1].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference lightingColorRef = {};
    lightingColorRef.attachment = 3;
    lightingColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpasses[2] = {};
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 2;
    subpasses[0].pColorAttachments = geometryColorRefs;
    subpasses[0].pDepthStencilAttachment = &geometryDepthRef;

    subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[1].colorAttachmentCount = 1;
    subpasses[1].pColorAttachments = &lightingColorRef;
    subpasses[1].inputAttachmentCount = 2;
    subpasses[1].pInputAttachments = inputRefs;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 4;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 2;
    renderPassInfo.pSubpasses = subpasses;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &outRenderPass));
}

static void createSwapchainTargets(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    const VkImageUsageFlags attachmentUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, GBUFFER_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.gbufferAlbedo);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, GBUFFER_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.gbufferNormal);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.gbufferDepth);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, GBUFFER_FORMAT,
                            attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.litTexture);
    for (int i = 0; i < 2; ++i) {
        createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, GBUFFER_FORMAT,
                                attachmentUsage, VK_IMAGE_ASPECT_COLOR_BIT, renderer.pingPong[i]);
    }

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.width = ctx.swapchainExtent.width;
    framebufferInfo.height = ctx.swapchainExtent.height;
    framebufferInfo.layers = 1;

    VkImageView geometryViews[3] = { renderer.gbufferAlbedo.view, renderer.gbufferNormal.view,
                                     renderer.gbufferDepth.view };
    framebufferInfo.renderPass = renderer.geometryRenderPass;
    framebufferInfo.attachmentCount = 3;
    framebufferInfo.pAttachments = geometryViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.geometryFramebuffer));

    VkImageView lightingViews[1] = { renderer.litTexture.view };
    framebufferInfo.renderPass = renderer.lightingRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = lightingViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.lightingFramebuffer));

    VkImageView subpassViews[4] = { renderer.gbufferAlbedo.view, renderer.gbufferNormal.view,
                                    renderer.gbufferDepth.view, renderer.litTexture.view };
    framebufferInfo.renderPass = renderer.subpassRenderPass;
    framebufferInfo.attachmentCount = 4;
    framebufferInfo.pAttachments = subpassViews;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.subpassFramebuffer));

    for (int i = 0; i < 2; ++i) {
        VkImageView views[1] = { renderer.pingPong[i].view };
        framebufferInfo.renderPass = renderer.pingPongRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = views;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.pingPongFramebuffer[i]));
    }

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

static void destroySwapchainTargets(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    for (int i = 0; i < 2; ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.pingPongFramebuffer[i], nullptr);
        renderer.pingPongFramebuffer[i] = VK_NULL_HANDLE;
        destroyTexture(ctx, renderer.pingPong[i]);
    }
    vkDestroyFramebuffer(ctx.device, renderer.subpassFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.lightingFramebuffer, nullptr);
    vkDestroyFramebuffer(ctx.device, renderer.geometryFramebuffer, nullptr);
    renderer.subpassFramebuffer = VK_NULL_HANDLE;
    renderer.lightingFramebuffer = VK_NULL_HANDLE;
    renderer.geometryFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.litTexture);
    destroyTexture(ctx, renderer.gbufferDepth);
    destroyTexture(ctx, renderer.gbufferNormal);
    destroyTexture(ctx, renderer.gbufferAlbedo);
}

static void createDescriptorLayouts(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    VkDescriptorSetLayoutBinding bindings[5] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    for (uint32_t i = 2; i <= 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.textureSetLayout));

    VkDescriptorSetLayoutBinding inputBindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        inputBindings[i].binding = i;
        inputBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        inputBindings[i].descriptorCount = 1;
        inputBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo inputLayoutInfo = {};
    inputLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    inputLayoutInfo.bindingCount = 2;
    inputLayoutInfo.pBindings = inputBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &inputLayoutInfo, nullptr, &renderer.inputSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 16;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 32;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));

    // 输入附件的描述符集不属于上面这个池的尺寸统计，单独用一个池
    VkDescriptorPoolSize inputPoolSize = {};
    inputPoolSize.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    inputPoolSize.descriptorCount = 8;

    VkDescriptorPoolCreateInfo inputPoolInfo = {};
    inputPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    inputPoolInfo.maxSets = 8;
    inputPoolInfo.poolSizeCount = 1;
    inputPoolInfo.pPoolSizes = &inputPoolSize;
    VkDescriptorPool inputPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &inputPoolInfo, nullptr, &inputPool));
    // 输入附件的描述符集在这里立刻分配好，随后销毁这个临时池的句柄登记
    renderer.inputDescriptorPool = inputPool;
}

static void writeDescriptors(const VulkanContext& ctx, SubpassRenderer& renderer, uint32_t index)
{
    SubpassFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.geometrySet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.geometrySet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);

    writeBufferDescriptor(ctx, frame.lightingSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeImageDescriptor(ctx, frame.lightingSet, 2, renderer.gbufferAlbedo.view, renderer.linearSampler);
    writeImageDescriptor(ctx, frame.lightingSet, 3, renderer.gbufferNormal.view, renderer.linearSampler);

    for (int i = 0; i < 2; ++i) {
        writeBufferDescriptor(ctx, frame.pingSets[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              frame.sceneBuffer);
        writeImageDescriptor(ctx, frame.pingSets[i], 4,
                             i == 0 ? renderer.litTexture.view : renderer.pingPong[0].view,
                             renderer.linearSampler);
    }

    writeBufferDescriptor(ctx, frame.presentSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeImageDescriptor(ctx, frame.presentSet, 4, renderer.litTexture.view, renderer.linearSampler);
    for (int i = 0; i < 2; ++i) {
        // 乒乓之后的最终结果在两张乒乓贴图中的一张上，输出通道按次数选对应的描述符集
        writeBufferDescriptor(ctx, frame.presentPingSets[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                              frame.sceneBuffer);
        writeImageDescriptor(ctx, frame.presentPingSets[i], 4, renderer.pingPong[i].view,
                             renderer.linearSampler);
    }

    VkDescriptorImageInfo imageInfos[2] = {};
    imageInfos[0].imageView = renderer.gbufferAlbedo.view;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = renderer.gbufferNormal.view;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.inputSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);
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
                                         VkRenderPass renderPass, uint32_t subpass,
                                         const char* vertexShader, const char* fragmentShader,
                                         uint32_t attachmentCount, bool depthTest)
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

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
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
    pipelineInfo.subpass = subpass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    return pipeline;
}

void createRenderer(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    renderer = SubpassRenderer();

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

    createGeometryRenderPass(ctx, renderer);
    createSingleColorRenderPass(ctx, GBUFFER_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                renderer.lightingRenderPass);
    createSubpassRenderPass(ctx, renderer, VK_ATTACHMENT_STORE_OP_DONT_CARE, renderer.subpassRenderPass);
    createSubpassRenderPass(ctx, renderer, VK_ATTACHMENT_STORE_OP_STORE, renderer.subpassRenderPassStored);
    createSingleColorRenderPass(ctx, GBUFFER_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                renderer.pingPongRenderPass);
    createSingleColorRenderPass(ctx, ctx.swapchainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                renderer.outputRenderPass);

    createDescriptorLayouts(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.textureSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.effectPipelineLayout));

    layoutInfo.pSetLayouts = &renderer.inputSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.inputPipelineLayout));

    renderer.geometryPipeline = createGraphicsPipeline(ctx, renderer.scenePipelineLayout,
                                                       renderer.geometryRenderPass, 0,
                                                       DEMO_SHADER_DIR "/gbuffer.vert.spv",
                                                       DEMO_SHADER_DIR "/gbuffer.frag.spv", 2, true);
    renderer.lightingPipeline = createGraphicsPipeline(ctx, renderer.scenePipelineLayout,
                                                       renderer.lightingRenderPass, 0,
                                                       DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                       DEMO_SHADER_DIR "/deferred_lighting.frag.spv", 1,
                                                       false);
    renderer.subpassGeometryPipeline = createGraphicsPipeline(ctx, renderer.scenePipelineLayout,
                                                               renderer.subpassRenderPass, 0,
                                                               DEMO_SHADER_DIR "/gbuffer.vert.spv",
                                                               DEMO_SHADER_DIR "/gbuffer.frag.spv", 2, true);
    renderer.subpassGeometryStoredPipeline =
        createGraphicsPipeline(ctx, renderer.scenePipelineLayout, renderer.subpassRenderPassStored, 0,
                               DEMO_SHADER_DIR "/gbuffer.vert.spv", DEMO_SHADER_DIR "/gbuffer.frag.spv", 2,
                               true);
    renderer.subpassLightingPipeline = createGraphicsPipeline(ctx, renderer.inputPipelineLayout,
                                                              renderer.subpassRenderPass, 1,
                                                              DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                              DEMO_SHADER_DIR "/subpass_lighting.frag.spv",
                                                              1, false);
    renderer.subpassLightingStoredPipeline =
        createGraphicsPipeline(ctx, renderer.inputPipelineLayout, renderer.subpassRenderPassStored, 1,
                               DEMO_SHADER_DIR "/fullscreen.vert.spv",
                               DEMO_SHADER_DIR "/subpass_lighting.frag.spv", 1, false);
    renderer.pingPongPipeline = createGraphicsPipeline(ctx, renderer.effectPipelineLayout,
                                                       renderer.pingPongRenderPass, 0,
                                                       DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                       DEMO_SHADER_DIR "/pingpong.frag.spv", 1, false);
    renderer.presentPipeline = createGraphicsPipeline(ctx, renderer.effectPipelineLayout,
                                                      renderer.outputRenderPass, 0,
                                                      DEMO_SHADER_DIR "/fullscreen.vert.spv",
                                                      DEMO_SHADER_DIR "/present.frag.spv", 1, false);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SubpassFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(SubpassSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.geometrySet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.lightingSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.pingSets[0] = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.pingSets[1] = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.presentSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.presentPingSets[0] =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.presentPingSets[1] =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.textureSetLayout);
        frame.inputSet = allocateDescriptorSet(ctx, renderer.inputDescriptorPool, renderer.inputSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SubpassFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.pingPongPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.subpassLightingStoredPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.subpassLightingPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.subpassGeometryStoredPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.subpassGeometryPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.lightingPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.geometryPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.effectPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.inputPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.inputDescriptorPool, nullptr);
    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.inputSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.textureSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.pingPongRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.subpassRenderPassStored, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.subpassRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.lightingRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.geometryRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.instanceBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, SubpassRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, SubpassRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SubpassSceneUniform& uniform, FrameStatistics& outStatistics)
{
    SubpassFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.sceneBuffer.mapped, &uniform, sizeof(SubpassSceneUniform));

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

    const bool subpassPath = input.options.path == SUBPASS_PATH_SUBPASS;
    const uint32_t pingPongCount = std::min(input.options.pingPongCount, 8u);
    uint32_t drawCallCount = 0;
    uint32_t renderPassCount = 0;

    VkClearValue geometryClear[3] = {};
    geometryClear[1].color.float32[2] = 1.0f;
    geometryClear[1].color.float32[3] = 1.0f;
    geometryClear[2].depthStencil.depth = 1.0f;

    const double geometryStart = nowSeconds();
    VkRenderPassBeginInfo geometryBegin = {};
    geometryBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geometryBegin.renderPass = subpassPath ? (input.options.transientGeometry
                                                  ? renderer.subpassRenderPass
                                                  : renderer.subpassRenderPassStored)
                                           : renderer.geometryRenderPass;
    geometryBegin.framebuffer = subpassPath ? renderer.subpassFramebuffer : renderer.geometryFramebuffer;
    geometryBegin.renderArea.extent = ctx.swapchainExtent;
    geometryBegin.clearValueCount = subpassPath ? 4 : 3;
    geometryBegin.pClearValues = geometryClear;

    vkCmdBeginRenderPass(frame.commandBuffer, &geometryBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer.scenePipelineLayout, 0, 1, &frame.geometrySet, 0, nullptr);
    VkPipeline geometryPipeline = renderer.geometryPipeline;
    if (subpassPath) {
        geometryPipeline = input.options.transientGeometry ? renderer.subpassGeometryPipeline
                                                           : renderer.subpassGeometryStoredPipeline;
    }
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, geometryPipeline);
    vkCmdDraw(frame.commandBuffer, 4, renderer.instanceCount, 0, 0);
    ++drawCallCount;

    if (subpassPath) {
        // 光照子通道与几何在同一个渲染通道里，几何缓冲没有被解析到主存
        vkCmdNextSubpass(frame.commandBuffer, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.inputPipelineLayout, 0, 1, &frame.inputSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          input.options.transientGeometry ? renderer.subpassLightingPipeline
                                                          : renderer.subpassLightingStoredPipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
    }
    vkCmdEndRenderPass(frame.commandBuffer);
    ++renderPassCount;
    outStatistics.cpuRecordGeometryMilliseconds = (nowSeconds() - geometryStart) * 1000.0;

    const double lightingStart = nowSeconds();
    if (!subpassPath) {
        VkRenderPassBeginInfo lightingBegin = {};
        lightingBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        lightingBegin.renderPass = renderer.lightingRenderPass;
        lightingBegin.framebuffer = renderer.lightingFramebuffer;
        lightingBegin.renderArea.extent = ctx.swapchainExtent;
        lightingBegin.clearValueCount = 0;

        vkCmdBeginRenderPass(frame.commandBuffer, &lightingBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.scenePipelineLayout, 0, 1, &frame.lightingSet, 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.lightingPipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
        ++renderPassCount;
    }
    outStatistics.cpuRecordLightingMilliseconds = (nowSeconds() - lightingStart) * 1000.0;

    // 乒乓：每一趟都把上一张结果采样回来再写出，制造新的渲染通道边界与附件依赖
    const double pingPongStart = nowSeconds();
    for (uint32_t i = 0; i < pingPongCount; ++i) {
        const uint32_t target = i % 2;
        VkRenderPassBeginInfo pingBegin = {};
        pingBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pingBegin.renderPass = renderer.pingPongRenderPass;
        pingBegin.framebuffer = renderer.pingPongFramebuffer[target];
        pingBegin.renderArea.extent = ctx.swapchainExtent;
        pingBegin.clearValueCount = 0;

        vkCmdBeginRenderPass(frame.commandBuffer, &pingBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer.effectPipelineLayout, 0, 1, &frame.pingSets[target], 0, nullptr);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.pingPongPipeline);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        ++drawCallCount;
        vkCmdEndRenderPass(frame.commandBuffer);
        ++renderPassCount;
    }
    outStatistics.cpuRecordPingPongMilliseconds = (nowSeconds() - pingPongStart) * 1000.0;

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
                            renderer.effectPipelineLayout, 0, 1,
                            pingPongCount > 0
                                ? &frame.presentPingSets[(pingPongCount - 1) % 2]
                                : &frame.presentSet,
                            0, nullptr);
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
