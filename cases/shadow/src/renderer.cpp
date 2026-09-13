#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <cstddef>
#include <cstring>
#include <vector>

static const VkFormat MAIN_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

// 阴影通道的深度测试附件，只用来挑出离光源最近的背面，不被采样
static const VkFormat SHADOW_TEST_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

VkFormat shadowMapColorFormat(uint32_t bits)
{
    if (bits == 8) {
        return VK_FORMAT_R8_UNORM;
    }
    if (bits == 16) {
        return VK_FORMAT_R16_UNORM;
    }
    if (bits == 32) {
        return VK_FORMAT_R32_SFLOAT;
    }
    FATAL("unsupported shadow map bit depth: %u", bits);
    return VK_FORMAT_UNDEFINED;
}

static void createShadowSampler(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    // 深度值保存在颜色附件里，采样器不做硬件比较，主通道逐纹素取值后在着色器里手动比较。
    // 滤波必须是最近邻，否则会把相邻纹素的深度值平均掉
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // 贴图范围之外取最远深度，手动比较时判定为受光
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.shadowSampler));
}

static void createShadowRenderPass(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};

    // 保存深度值的颜色附件，主通道以采样方式读取
    attachments[0].format = shadowMapColorFormat(renderer.shadowMapBits);
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 阴影通道结束后主通道以采样方式读取，布局在这里就转到只读
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 只用于深度测试的深度附件，内容不需要保留
    attachments[1].format = SHADOW_TEST_DEPTH_FORMAT;
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

    // 上一帧主通道读过阴影贴图，写入前先等它读完；写完之后再交给片元着色器采样
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.shadowRenderPass));
}

static void createMainRenderPass(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = ctx.swapchainFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = MAIN_DEPTH_FORMAT;
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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.mainRenderPass));
}

static void createShadowTargets(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    const uint32_t size = renderer.shadowMapSize;

    createAttachmentTexture(ctx, size, size, shadowMapColorFormat(renderer.shadowMapBits),
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.shadowMap);
    createAttachmentTexture(ctx, size, size, SHADOW_TEST_DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.shadowDepth);

    VkImageView views[2] = { renderer.shadowMap.view, renderer.shadowDepth.view };
    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.shadowRenderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = size;
    framebufferInfo.height = size;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.shadowFramebuffer));

    // 创建时先转到只读布局：关闭阴影时不跑阴影通道，主通道仍然绑定着这张贴图，
    // 布局必须与描述符里声明的只读布局一致
    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = renderer.shadowMap.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    endOneTimeCommands(ctx, commandBuffer);
}

static void destroyShadowTargets(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    vkDestroyFramebuffer(ctx.device, renderer.shadowFramebuffer, nullptr);
    renderer.shadowFramebuffer = VK_NULL_HANDLE;
    destroyTexture(ctx, renderer.shadowDepth);
    destroyTexture(ctx, renderer.shadowMap);
}

static void createSwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, MAIN_DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.depthTexture);

    renderer.presentFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView views[2] = { ctx.swapchainImageViews[i], renderer.depthTexture.view };

        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderer.mainRenderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = ctx.swapchainExtent.width;
        framebufferInfo.height = ctx.swapchainExtent.height;
        framebufferInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.presentFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.presentFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.presentFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.presentFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.depthTexture);
}

static void createDescriptorLayouts(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkDescriptorSetLayoutBinding sceneBindings[2] = {};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[1].descriptorCount = 1;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo sceneLayoutInfo = {};
    sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sceneLayoutInfo.bindingCount = 2;
    sceneLayoutInfo.pBindings = sceneBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &sceneLayoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorSetLayoutBinding materialBindings[6] = {};
    for (int i = 0; i < 6; ++i) {
        materialBindings[i].binding = static_cast<uint32_t>(i);
        materialBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialBindings[i].descriptorCount = 1;
        materialBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo materialLayoutInfo = {};
    materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    materialLayoutInfo.bindingCount = 6;
    materialLayoutInfo.pBindings = materialBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &materialLayoutInfo, nullptr, &renderer.materialSetLayout));
}

static void createDescriptorPool(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

// 位置、法线、纹理坐标三个顶点属性，两个管道共用
static void fillVertexInput(VkVertexInputBindingDescription& binding,
                            VkVertexInputAttributeDescription (&attributes)[3])
{
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = static_cast<uint32_t>(offsetof(MeshVertex, position));
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = static_cast<uint32_t>(offsetof(MeshVertex, normal));
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = static_cast<uint32_t>(offsetof(MeshVertex, uv));
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

static void createShadowPipeline(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.shadowPipelineLayout));

    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/shadow.vert.spv"));
    VkShaderModule fragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/shadow.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
    // 深度值由片元着色器写进颜色附件，主通道再以采样方式读取
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);

    VkVertexInputBindingDescription vertexBinding = {};
    VkVertexInputAttributeDescription vertexAttributes[3] = {};
    fillVertexInput(vertexBinding, vertexAttributes);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    // 阴影通道只读位置
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = vertexAttributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    // 颜色附件只保存深度值，写入 R 通道
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // 物体与地面各一条管线，差别只在剔除方式：地面是单面几何，不做剔除
    for (int pass = 0; pass < 2; ++pass) {
        const bool groundPass = pass == 1;

        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        // 只写入背面时，主通道里的受光表面稳定处在阴影贴图深度之前，用物体厚度换来深度余量
        rasterization.cullMode = groundPass
                                     ? VK_CULL_MODE_NONE
                                     : (renderer.shadowBackFaceDepth ? VK_CULL_MODE_FRONT_BIT
                                                                     : VK_CULL_MODE_BACK_BIT);
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        // 贴图里的深度由片元着色器写进颜色附件，光栅化的深度偏移只作用在深度测试上，
        // 影响不到写入值，因此这里不启用

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
        pipelineInfo.layout = renderer.shadowPipelineLayout;
        pipelineInfo.renderPass = renderer.shadowRenderPass;
        pipelineInfo.subpass = 0;

        VkPipeline* targetPipeline =
            groundPass ? &renderer.shadowGroundPipeline : &renderer.shadowPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           targetPipeline));
    }

    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

// 物体与地面共用顶点着色器，只有片元着色器与剔除方式不同：地面是单面几何，不做背面剔除
static void createMainPipelines(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    VkDescriptorSetLayout setLayouts[2] = { renderer.sceneSetLayout, renderer.materialSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));

    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.vert.spv"));
    VkShaderModule sceneFragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/scene.frag.spv"));
    VkShaderModule groundFragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/ground.frag.spv"));

    VkVertexInputBindingDescription vertexBinding = {};
    VkVertexInputAttributeDescription vertexAttributes[3] = {};
    fillVertexInput(vertexBinding, vertexAttributes);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = vertexAttributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

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

    for (int pass = 0; pass < 2; ++pass) {
        const bool groundPass = pass == 1;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
        stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT,
                                    groundPass ? groundFragmentModule : sceneFragmentModule);

        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = groundPass ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

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
        pipelineInfo.renderPass = renderer.mainRenderPass;
        pipelineInfo.subpass = 0;

        VkPipeline* targetPipeline = groundPass ? &renderer.groundPipeline : &renderer.scenePipeline;
        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           targetPipeline));
    }

    vkDestroyShaderModule(ctx.device, groundFragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, sceneFragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

static void createMeshBuffers(const VulkanContext& ctx, const MeshData& mesh, GpuBuffer& vertexBuffer,
                              GpuBuffer& indexBuffer, uint32_t& indexCount)
{
    indexCount = static_cast<uint32_t>(mesh.indices.size());

    const VkDeviceSize vertexBytes = sizeof(MeshVertex) * mesh.vertices.size();
    createBuffer(ctx, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertexBuffer);
    uploadBufferData(ctx, vertexBuffer, mesh.vertices.data(), vertexBytes);

    const VkDeviceSize indexBytes = sizeof(uint32_t) * mesh.indices.size();
    createBuffer(ctx, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, indexBuffer);
    uploadBufferData(ctx, indexBuffer, mesh.indices.data(), indexBytes);
}

void createRenderer(const VulkanContext& ctx, ShadowRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t groundInstanceIndex, const ShadowOptions& shadowOptions)
{
    renderer = ShadowRenderer();
    renderer.instanceCapacity = static_cast<uint32_t>(instances.size());
    renderer.groundInstanceIndex = groundInstanceIndex;
    renderer.shadowMapSize = shadowOptions.mapSize;
    renderer.shadowMapBits = shadowOptions.mapBits;
    renderer.shadowBackFaceDepth = shadowOptions.backFaceDepth;

    // 时间戳查询支持由设备能力决定，桌面与安卓都按同一套规则探测
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    createMeshBuffers(ctx, objectMesh, renderer.objectVertexBuffer, renderer.objectIndexBuffer,
                      renderer.objectIndexCount);
    createMeshBuffers(ctx, groundMesh, renderer.groundVertexBuffer, renderer.groundIndexBuffer,
                      renderer.groundIndexCount);

    const VkDeviceSize instanceBytes = sizeof(InstanceData) * instances.size();
    createBuffer(ctx, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.instanceBuffer);
    uploadBufferData(ctx, renderer.instanceBuffer, instances.data(), instanceBytes);

    createBackpackMaterialTextures(ctx, renderer.material);

    createShadowSampler(ctx, renderer);
    createShadowRenderPass(ctx, renderer);
    createMainRenderPass(ctx, renderer);
    createShadowTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createShadowPipeline(ctx, renderer);
    createMainPipelines(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    renderer.materialSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.materialSetLayout);
    writeImageDescriptor(ctx, renderer.materialSet, 0, renderer.material.albedo.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 1, renderer.material.normal.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 2, renderer.material.metallic.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 3, renderer.material.roughness.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 4, renderer.material.ambientOcclusion.view,
                         renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 5, renderer.shadowMap.view, renderer.shadowSampler);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ShadowFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(ShadowSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
        writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
        writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              renderer.instanceBuffer);

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

void destroyRenderer(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ShadowFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.groundPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scenePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.shadowGroundPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.shadowPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.shadowPipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.materialSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    destroyShadowTargets(ctx, renderer);
    vkDestroySampler(ctx.device, renderer.shadowSampler, nullptr);

    vkDestroyRenderPass(ctx.device, renderer.mainRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.shadowRenderPass, nullptr);

    destroyBackpackMaterialTextures(ctx, renderer.material);

    destroyBuffer(ctx, renderer.instanceBuffer);
    destroyBuffer(ctx, renderer.groundIndexBuffer);
    destroyBuffer(ctx, renderer.groundVertexBuffer);
    destroyBuffer(ctx, renderer.objectIndexBuffer);
    destroyBuffer(ctx, renderer.objectVertexBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, ShadowRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
}

// 阴影配置改变时重建相应资源：尺寸与位数决定渲染通道的附件格式、帧缓冲与纹理，
// 只写背面深度决定阴影管线的剔除面，最后把新的贴图视图写回材质描述符的绑定 5
static void applyShadowOptions(const VulkanContext& ctx, ShadowRenderer& renderer, const ShadowOptions& options)
{
    const bool targetsChanged = renderer.shadowMapSize != options.mapSize ||
                                renderer.shadowMapBits != options.mapBits;
    const bool pipelineChanged = renderer.shadowBackFaceDepth != options.backFaceDepth;
    if (!targetsChanged && !pipelineChanged) {
        return;
    }

    // 旧的贴图与管线可能还被另一帧使用，先等设备空闲再拆
    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    vkDestroyPipeline(ctx.device, renderer.shadowGroundPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.shadowPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.shadowPipelineLayout, nullptr);

    if (targetsChanged) {
        vkDestroyRenderPass(ctx.device, renderer.shadowRenderPass, nullptr);
        destroyShadowTargets(ctx, renderer);

        renderer.shadowMapSize = options.mapSize;
        renderer.shadowMapBits = options.mapBits;

        createShadowRenderPass(ctx, renderer);
        createShadowTargets(ctx, renderer);
        writeImageDescriptor(ctx, renderer.materialSet, 5, renderer.shadowMap.view, renderer.shadowSampler);
    }

    renderer.shadowBackFaceDepth = options.backFaceDepth;
    createShadowPipeline(ctx, renderer);
}

bool drawFrame(const VulkanContext& ctx, ShadowRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const ShadowSceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    ShadowFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    applyShadowOptions(ctx, renderer, input.shadow);

    // 上一次使用本组资源的那一帧已经完成，可以读取它的计时
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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(ShadowSceneUniform));

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

    VkDeviceSize vertexOffset = 0;
    uint32_t drawCallCount = 0;

    const double shadowPassStart = nowSeconds();
    if (input.shadowMode != SHADOW_MODE_OFF) {
        const float shadowMapSize = static_cast<float>(renderer.shadowMapSize);

        VkViewport shadowViewport = {};
        shadowViewport.width = shadowMapSize;
        shadowViewport.height = shadowMapSize;
        shadowViewport.maxDepth = 1.0f;

        VkRect2D shadowScissor = {};
        shadowScissor.extent.width = renderer.shadowMapSize;
        shadowScissor.extent.height = renderer.shadowMapSize;

        // 颜色附件的初值取最远深度，落在贴图外的像素由此判定为受光；深度附件同为首帧清成最远
        VkClearValue shadowClearValues[2] = {};
        shadowClearValues[0].color.float32[0] = 1.0f;
        shadowClearValues[0].color.float32[1] = 0.0f;
        shadowClearValues[0].color.float32[2] = 0.0f;
        shadowClearValues[0].color.float32[3] = 1.0f;
        shadowClearValues[1].depthStencil.depth = 1.0f;

        VkRenderPassBeginInfo shadowBegin = {};
        shadowBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        shadowBegin.renderPass = renderer.shadowRenderPass;
        shadowBegin.framebuffer = renderer.shadowFramebuffer;
        shadowBegin.renderArea.extent.width = renderer.shadowMapSize;
        shadowBegin.renderArea.extent.height = renderer.shadowMapSize;
        shadowBegin.clearValueCount = 2;
        shadowBegin.pClearValues = shadowClearValues;

        vkCmdBeginRenderPass(frame.commandBuffer, &shadowBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &shadowViewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &shadowScissor);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.shadowPipeline);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.shadowPipelineLayout,
                                0, 1, &frame.sceneSet, 0, nullptr);
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.objectVertexBuffer.buffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, renderer.objectIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(frame.commandBuffer, renderer.objectIndexCount, input.activeInstanceCount, 0, 0, 0);
        ++drawCallCount;

        if (input.shadow.groundCaster) {
            // 地面写进贴图后会与自身比较，受光比例在平坦表面上按深度量化结果跳变，形成条纹。
            // 地面的实例变换同样放在实例缓冲末尾，用 firstInstance 指向它
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              renderer.shadowGroundPipeline);
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.groundVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(frame.commandBuffer, renderer.groundIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.commandBuffer, renderer.groundIndexCount, 1, 0, 0,
                             renderer.groundInstanceIndex);
            ++drawCallCount;
        }

        vkCmdEndRenderPass(frame.commandBuffer);
    }
    outStatistics.cpuRecordShadowPassMilliseconds = (nowSeconds() - shadowPassStart) * 1000.0;

    const double mainPassStart = nowSeconds();

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    VkClearValue mainClearValues[2] = {};
    mainClearValues[0].color.float32[0] = 0.02f;
    mainClearValues[0].color.float32[1] = 0.025f;
    mainClearValues[0].color.float32[2] = 0.035f;
    mainClearValues[0].color.float32[3] = 1.0f;
    mainClearValues[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo mainBegin = {};
    mainBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    mainBegin.renderPass = renderer.mainRenderPass;
    mainBegin.framebuffer = renderer.presentFramebuffers[imageIndex];
    mainBegin.renderArea.extent = ctx.swapchainExtent;
    mainBegin.clearValueCount = 2;
    mainBegin.pClearValues = mainClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &mainBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    VkDescriptorSet mainSets[2] = { frame.sceneSet, renderer.materialSet };

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePipeline);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePipelineLayout,
                            0, 2, mainSets, 0, nullptr);
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.objectVertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.objectIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(frame.commandBuffer, renderer.objectIndexCount, input.activeInstanceCount, 0, 0, 0);
    ++drawCallCount;

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.groundPipeline);
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.groundVertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.groundIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    // 地面的实例变换放在实例缓冲的末尾，用 firstInstance 指向它
    vkCmdDrawIndexed(frame.commandBuffer, renderer.groundIndexCount, 1, 0, 0, renderer.groundInstanceIndex);
    ++drawCallCount;

    outStatistics.cpuRecordMainPassMilliseconds = (nowSeconds() - mainPassStart) * 1000.0;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(frame.commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    // 抓帧拷贝在渲染通道之外进行，图像屏障不能出现在渲染通道内部
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
