#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <cstddef>
#include <cstring>
#include <vector>

// 深度附件可选三种格式。Reverse-Z 依赖浮点深度在靠近 0 的一侧仍有足够的相对精度，
// 定点格式的精度沿整个深度范围均匀分布，换不换方向都一样
VkFormat depthFormatForOption(uint32_t option)
{
    if (option == DEPTH_FORMAT_OPTION_D16) {
        return VK_FORMAT_D16_UNORM;
    }
    if (option == DEPTH_FORMAT_OPTION_D24) {
        return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_D32_SFLOAT;
}

float depthFormatQuantizeLevels(uint32_t option)
{
    if (option == DEPTH_FORMAT_OPTION_D16) {
        return 65535.0f;
    }
    if (option == DEPTH_FORMAT_OPTION_D24) {
        return 16777215.0f;
    }
    // 浮点格式不做量化
    return 0.0f;
}

const char* depthFormatName(uint32_t option)
{
    if (option == DEPTH_FORMAT_OPTION_D16) {
        return "D16_UNORM";
    }
    if (option == DEPTH_FORMAT_OPTION_D24) {
        return "D24_UNORM_S8";
    }
    return "D32_SFLOAT";
}

// 标准深度把近平面映射到 0、远平面映射到 1，越近的值越容易通过，判定用小于；
// Reverse-Z 把近平面映射到 1、远平面映射到 0，越近的值越大，判定改用大于
static VkCompareOp depthCompareOp(bool reverseZ)
{
    return reverseZ ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
}

static VkClearDepthStencilValue depthClearValue(bool reverseZ)
{
    VkClearDepthStencilValue value = {};
    value.depth = reverseZ ? 0.0f : 1.0f;
    return value;
}

static void createRenderPass(const VulkanContext& ctx, ReverseZRenderer& renderer)
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

    attachments[1].format = renderer.depthFormat;
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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.renderPass));
}

static void createSwapchainTargets(const VulkanContext& ctx, ReverseZRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, renderer.depthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                            renderer.depthTexture);

    renderer.presentFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView views[2] = { ctx.swapchainImageViews[i], renderer.depthTexture.view };

        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderer.renderPass;
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

static void destroySwapchainTargets(const VulkanContext& ctx, ReverseZRenderer& renderer)
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

static void createDescriptorLayouts(const VulkanContext& ctx, ReverseZRenderer& renderer)
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

    // 物体只用 albedo 一张贴图，其余几张是公共资源读取函数一并创建的
    VkDescriptorSetLayoutBinding materialBinding = {};
    materialBinding.binding = 0;
    materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBinding.descriptorCount = 1;
    materialBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo materialLayoutInfo = {};
    materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    materialLayoutInfo.bindingCount = 1;
    materialLayoutInfo.pBindings = &materialBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &materialLayoutInfo, nullptr, &renderer.materialSetLayout));
}

static void createDescriptorPool(const VulkanContext& ctx, ReverseZRenderer& renderer)
{
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 8;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 16;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

// 位置、法线、纹理坐标三个顶点属性，两条管线共用
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

// 两条管线的差别只有着色器模块与剔除面，其余状态一致。深度判定由 reverseZ 决定，
// 切换深度模式时重建管线；颜色附件都写满四个通道
static void createMainPipelines(const VulkanContext& ctx, ReverseZRenderer& renderer)
{
    VkDescriptorSetLayout setLayouts[2] = { renderer.sceneSetLayout, renderer.materialSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.pipelineLayout));

    VkShaderModule groundVertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/ground.vert.spv"));
    VkShaderModule groundFragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/ground.frag.spv"));
    VkShaderModule objectVertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/object.vert.spv"));
    VkShaderModule objectFragmentModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/object.frag.spv"));

    VkVertexInputBindingDescription vertexBinding = {};
    VkVertexInputAttributeDescription vertexAttributes[3] = {};
    fillVertexInput(vertexBinding, vertexAttributes);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
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
    depthStencil.depthCompareOp = depthCompareOp(renderer.reverseZ);

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

    VkShaderModule vertexModules[2] = { groundVertexModule, objectVertexModule };
    VkShaderModule fragmentModules[2] = { groundFragmentModule, objectFragmentModule };
    VkCullModeFlags cullModes[2] = { VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT };
    // 地面只用位置，物体用满位置、法线与纹理坐标三个属性
    uint32_t attributeCounts[2] = { 1, 3 };
    VkPipeline* targetPipelines[2] = { &renderer.groundPipeline, &renderer.objectPipeline };

    for (int pass = 0; pass < 2; ++pass) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModules[pass]);
        stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModules[pass]);

        vertexInput.vertexAttributeDescriptionCount = attributeCounts[pass];

        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        // 地面是单面几何，不做背面剔除；物体保留背面剔除
        rasterization.cullMode = cullModes[pass];
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
        pipelineInfo.layout = renderer.pipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        pipelineInfo.subpass = 0;

        VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                           targetPipelines[pass]));
    }

    vkDestroyShaderModule(ctx.device, objectFragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, objectVertexModule, nullptr);
    vkDestroyShaderModule(ctx.device, groundFragmentModule, nullptr);
    vkDestroyShaderModule(ctx.device, groundVertexModule, nullptr);
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

void createRenderer(const VulkanContext& ctx, ReverseZRenderer& renderer, const MeshData& objectMesh,
                    const MeshData& groundMesh, const std::vector<InstanceData>& instances,
                    uint32_t objectInstanceOffset, bool reverseZ, uint32_t depthFormatOption)
{
    renderer = ReverseZRenderer();
    renderer.reverseZ = reverseZ;
    renderer.depthFormatOption = depthFormatOption;
    renderer.depthFormat = depthFormatForOption(depthFormatOption);
    renderer.groundInstanceCount = GROUND_LAYER_COUNT;
    renderer.objectInstanceOffset = objectInstanceOffset;
    renderer.objectCapacity = static_cast<uint32_t>(instances.size()) - objectInstanceOffset;

    // 时间戳查询支持由设备能力决定，桌面与安卓都按同一套规则探测
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    createMeshBuffers(ctx, groundMesh, renderer.groundVertexBuffer, renderer.groundIndexBuffer,
                      renderer.groundIndexCount);
    createMeshBuffers(ctx, objectMesh, renderer.objectVertexBuffer, renderer.objectIndexBuffer,
                      renderer.objectIndexCount);

    const VkDeviceSize instanceBytes = sizeof(InstanceData) * instances.size();
    createBuffer(ctx, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.instanceBuffer);
    uploadBufferData(ctx, renderer.instanceBuffer, instances.data(), instanceBytes);

    createBackpackMaterialTextures(ctx, renderer.material);

    createRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createMainPipelines(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    renderer.materialSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.materialSetLayout);
    writeImageDescriptor(ctx, renderer.materialSet, 0, renderer.material.albedo.view, renderer.material.sampler);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ReverseZFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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

void destroyRenderer(const VulkanContext& ctx, ReverseZRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ReverseZFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.objectPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.groundPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.pipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.materialSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.renderPass, nullptr);

    destroyBackpackMaterialTextures(ctx, renderer.material);

    destroyBuffer(ctx, renderer.instanceBuffer);
    destroyBuffer(ctx, renderer.objectIndexBuffer);
    destroyBuffer(ctx, renderer.objectVertexBuffer);
    destroyBuffer(ctx, renderer.groundIndexBuffer);
    destroyBuffer(ctx, renderer.groundVertexBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, ReverseZRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
}

// 深度模式或深度附件格式切换后重建受影响的资源。深度判定方向只影响管线，改动后重建管线；
// 深度附件格式还会改变渲染通道的附件格式，改动后连渲染通道、深度附件与帧缓冲一起重建。
// 旧的资源可能还被另一帧使用，先等设备空闲再拆
static void applyRendererOptions(const VulkanContext& ctx, ReverseZRenderer& renderer, const FrameInput& input)
{
    const bool formatChanged = renderer.depthFormatOption != input.depthFormatOption;
    const bool pipeChanged = renderer.reverseZ != input.reverseZ;
    if (!formatChanged && !pipeChanged) {
        return;
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    vkDestroyPipeline(ctx.device, renderer.objectPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.groundPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.pipelineLayout, nullptr);

    renderer.reverseZ = input.reverseZ;

    if (formatChanged) {
        destroySwapchainTargets(ctx, renderer);
        vkDestroyRenderPass(ctx.device, renderer.renderPass, nullptr);

        renderer.depthFormatOption = input.depthFormatOption;
        renderer.depthFormat = depthFormatForOption(input.depthFormatOption);

        createRenderPass(ctx, renderer);
        createSwapchainTargets(ctx, renderer);
    }

    createMainPipelines(ctx, renderer);
}

bool drawFrame(const VulkanContext& ctx, ReverseZRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const SceneUniform& sceneUniform, FrameStatistics& outStatistics)
{
    ReverseZFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    applyRendererOptions(ctx, renderer, input);

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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(SceneUniform));

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
    mainClearValues[1].depthStencil = depthClearValue(renderer.reverseZ);

    VkRenderPassBeginInfo mainBegin = {};
    mainBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    mainBegin.renderPass = renderer.renderPass;
    mainBegin.framebuffer = renderer.presentFramebuffers[imageIndex];
    mainBegin.renderArea.extent = ctx.swapchainExtent;
    mainBegin.clearValueCount = 2;
    mainBegin.pClearValues = mainClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &mainBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    VkDescriptorSet mainSets[2] = { frame.sceneSet, renderer.materialSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.pipelineLayout, 0, 2,
                            mainSets, 0, nullptr);

    // 先画下层地面，再画上层。两层在远处的归一化深度会量化到同一个值，
    // 此时上层被判定为不更近而落在下层之后，这正是要观察的 Z-fighting
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.groundPipeline);
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.groundVertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.groundIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(frame.commandBuffer, renderer.groundIndexCount, renderer.groundInstanceCount, 0, 0, 0);
    ++drawCallCount;

    if (input.activeObjectCount > 0) {
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.objectPipeline);
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.objectVertexBuffer.buffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, renderer.objectIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(frame.commandBuffer, renderer.objectIndexCount, input.activeObjectCount, 0, 0,
                         renderer.objectInstanceOffset);
        ++drawCallCount;
    }

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
