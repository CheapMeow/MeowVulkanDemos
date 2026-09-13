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

const char* outlineModeName(uint32_t mode)
{
    if (mode == OUTLINE_POST) {
        return "post";
    }
    if (mode == OUTLINE_HULL) {
        return "hull";
    }
    return "none";
}

// 把一个顶点追加到顶点数组
static void pushVertex(std::vector<float>& vertices, const glm::vec3& position, const glm::vec3& normal)
{
    vertices.push_back(position.x);
    vertices.push_back(position.y);
    vertices.push_back(position.z);
    vertices.push_back(normal.x);
    vertices.push_back(normal.y);
    vertices.push_back(normal.z);
}

// 单位立方体：六个面各四个顶点，法线按面给出，双 Pass 外扩沿它们外推才能得到完整的一圈
static void buildBoxMesh(std::vector<float>& vertices, std::vector<uint16_t>& indices)
{
    const glm::vec3 normals[6] = { { 0, 0, 1 },  { 0, 0, -1 }, { 1, 0, 0 },
                                   { -1, 0, 0 }, { 0, 1, 0 },  { 0, -1, 0 } };
    for (int face = 0; face < 6; ++face) {
        const glm::vec3 n = normals[face];
        // 面内两个切向量
        glm::vec3 tangent = face < 2 ? glm::vec3(1, 0, 0) : (face < 4 ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
        const glm::vec3 bitangent = glm::cross(n, tangent);
        const uint16_t base = static_cast<uint16_t>(vertices.size() / 6);
        for (int corner = 0; corner < 4; ++corner) {
            const float su = (corner & 1) == 0 ? -0.5f : 0.5f;
            const float sv = (corner & 2) == 0 ? -0.5f : 0.5f;
            pushVertex(vertices, n * 0.5f + tangent * su + bitangent * sv, n);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

// 薄片：只有一个面，正面剔除之后什么都不剩，双 Pass 外扩对它失效
static void buildPlateMesh(std::vector<float>& vertices, std::vector<uint16_t>& indices)
{
    const glm::vec3 normal(0.0f, 0.0f, 1.0f);
    const uint16_t base = static_cast<uint16_t>(vertices.size() / 6);
    for (int corner = 0; corner < 4; ++corner) {
        const float su = (corner & 1) == 0 ? -0.5f : 0.5f;
        const float sv = (corner & 2) == 0 ? -0.5f : 0.5f;
        pushVertex(vertices, glm::vec3(su, sv, 0.0f), normal);
    }
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

// 场景：三排立方体加一排薄片
static void buildScene(std::vector<OutlineInstance>& instances, uint32_t& boxCount, uint32_t& plateCount)
{
    instances.clear();
    boxCount = 0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 6; ++column) {
            const int index = row * 6 + column;
            const float x = -1.35f + static_cast<float>(column) * 0.54f;
            const float y = -0.42f + static_cast<float>(row) * 0.44f;
            const float z = -2.6f - 0.26f * static_cast<float>(index % 4);
            const float size = 0.34f + 0.05f * static_cast<float>(column % 3);
            const float t = static_cast<float>(index % 4) / 4.0f;
            instances.push_back({ glm::vec4(x, y, z, size), glm::vec4(0.30f + 0.5f * t, 0.55f, 0.85f - 0.5f * t, 0.0f) });
            ++boxCount;
        }
    }
    plateCount = 0;
    for (int i = 0; i < 8; ++i) {
        const float x = -1.19f + static_cast<float>(i) * 0.34f;
        const float z = -3.0f - 0.2f * static_cast<float>(i % 3);
        instances.push_back({ glm::vec4(x, 1.18f, z, 0.5f), glm::vec4(0.85f, 0.35f, 0.35f, 1.0f) });
        ++plateCount;
    }
}

// 场景通道：颜色、法线与深度三张附件
static void createSceneRenderPass(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    VkAttachmentDescription attachments[3] = {};
    const VkFormat formats[3] = { SCENE_COLOR_FORMAT, SCENE_NORMAL_FORMAT, SCENE_DEPTH_FORMAT };
    for (int i = 0; i < 3; ++i) {
        attachments[i].format = formats[i];
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

static void createOutputRenderPass(const VulkanContext& ctx, OutlineRenderer& renderer)
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

static void createSwapchainTargets(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_COLOR_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneColor);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_NORMAL_FORMAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.sceneNormal);
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, SCENE_DEPTH_FORMAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.sceneDepth);

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

static void destroySwapchainTargets(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    vkDestroyFramebuffer(ctx.device, renderer.sceneFramebuffer, nullptr);
    renderer.sceneFramebuffer = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < renderer.outputFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.outputFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.outputFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.sceneDepth);
    destroyTexture(ctx, renderer.sceneNormal);
    destroyTexture(ctx, renderer.sceneColor);
}

static void createDescriptorLayout(const VulkanContext& ctx, OutlineRenderer& renderer)
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
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 24;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static void writeDescriptors(const VulkanContext& ctx, OutlineRenderer& renderer, uint32_t index)
{
    OutlineFrameResources& frame = renderer.frames[index];
    writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.sceneBuffer);
    writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          renderer.instanceBuffer);
    writeImageDescriptor(ctx, frame.sceneSet, 2, renderer.sceneDepth.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 3, renderer.sceneNormal.view, renderer.nearestSampler);
    writeImageDescriptor(ctx, frame.sceneSet, 4, renderer.sceneColor.view, renderer.linearSampler);
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

// 场景管线：三套只有剔除方式不同，双 Pass 外扩还要反转正面
static VkPipeline createScenePipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                      VkRenderPass renderPass, const char* vertexShader,
                                      const char* fragmentShader, VkCullModeFlags cullMode)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(vertexShader));
    VkShaderModule fragmentModule = loadShaderModuleFromMemory(ctx, readAssetBytes(fragmentShader));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = makeShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertexModule);
    stages[1] = makeShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule);

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(float) * 6;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[2] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = sizeof(float) * 3;

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

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
    rasterization.cullMode = cullMode;
    // 顶点按从外面看的逆时针给出，帧缓冲的纵轴朝下，正面的判定要按顺时针
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 2;
    colorBlend.pAttachments = blendAttachments;

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

static VkPipeline createCompositePipeline(const VulkanContext& ctx, VkPipelineLayout layout,
                                          VkRenderPass renderPass, const char* fragmentShader)
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

void createRenderer(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    renderer = OutlineRenderer();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;

    std::vector<float> vertices;
    std::vector<uint16_t> indices;
    buildBoxMesh(vertices, indices);
    renderer.boxVertexCount = static_cast<uint32_t>(vertices.size() / 6);
    renderer.boxIndexCount = static_cast<uint32_t>(indices.size());
    renderer.plateVertexCount = 4;
    renderer.plateIndexCount = 6;
    renderer.plateIndexOffset = static_cast<uint32_t>(indices.size());
    buildPlateMesh(vertices, indices);

    createBuffer(ctx, sizeof(float) * vertices.size(),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.vertexBuffer);
    uploadBufferData(ctx, renderer.vertexBuffer, vertices.data(), sizeof(float) * vertices.size());

    createBuffer(ctx, sizeof(uint16_t) * indices.size(),
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.indexBuffer);
    uploadBufferData(ctx, renderer.indexBuffer, indices.data(), sizeof(uint16_t) * indices.size());

    std::vector<OutlineInstance> instances;
    buildScene(instances, renderer.boxInstanceCount, renderer.plateInstanceCount);
    createBuffer(ctx, sizeof(OutlineInstance) * instances.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 renderer.instanceBuffer);
    std::memcpy(renderer.instanceBuffer.mapped, instances.data(), sizeof(OutlineInstance) * instances.size());

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

    createSceneRenderPass(ctx, renderer);
    createOutputRenderPass(ctx, renderer);
    createDescriptorLayout(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.scenePipelineLayout));

    renderer.hullPipeline =
        createScenePipeline(ctx, renderer.scenePipelineLayout, renderer.sceneRenderPass,
                            DEMO_SHADER_DIR "/hull.vert.spv", DEMO_SHADER_DIR "/hull.frag.spv",
                            VK_CULL_MODE_BACK_BIT);
    renderer.sceneBoxPipeline =
        createScenePipeline(ctx, renderer.scenePipelineLayout, renderer.sceneRenderPass,
                            DEMO_SHADER_DIR "/scene.vert.spv", DEMO_SHADER_DIR "/scene.frag.spv",
                            VK_CULL_MODE_FRONT_BIT);
    renderer.scenePlatePipeline =
        createScenePipeline(ctx, renderer.scenePipelineLayout, renderer.sceneRenderPass,
                            DEMO_SHADER_DIR "/scene.vert.spv", DEMO_SHADER_DIR "/scene.frag.spv",
                            VK_CULL_MODE_NONE);

    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &renderer.sceneSetLayout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.compositePipelineLayout));
    renderer.outlinePipeline =
        createCompositePipeline(ctx, renderer.compositePipelineLayout, renderer.outputRenderPass,
                                DEMO_SHADER_DIR "/outline.frag.spv");
    renderer.presentPipeline =
        createCompositePipeline(ctx, renderer.compositePipelineLayout, renderer.outputRenderPass,
                                DEMO_SHADER_DIR "/present.frag.spv");

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        OutlineFrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(OutlineSceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.sceneBuffer);

        frame.sceneSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sceneSetLayout);
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

void destroyRenderer(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        OutlineFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.sceneBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.presentPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.outlinePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.compositePipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.scenePlatePipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.sceneBoxPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.hullPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.scenePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.outputRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.sceneRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.nearestSampler, nullptr);
    vkDestroySampler(ctx.device, renderer.linearSampler, nullptr);
    destroyBuffer(ctx, renderer.instanceBuffer);
    destroyBuffer(ctx, renderer.indexBuffer);
    destroyBuffer(ctx, renderer.vertexBuffer);
}

void recreateSwapchainTargets(const VulkanContext& ctx, OutlineRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeDescriptors(ctx, renderer, static_cast<uint32_t>(i));
    }
}

bool drawFrame(const VulkanContext& ctx, OutlineRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const OutlineSceneUniform& sceneUniform,
               FrameStatistics& outStatistics)
{
    OutlineFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

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

    std::memcpy(frame.sceneBuffer.mapped, &sceneUniform, sizeof(OutlineSceneUniform));

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

    const bool hullMode = input.options.mode == OUTLINE_HULL;
    uint32_t drawCallCount = 0;

    const double sceneStart = nowSeconds();

    VkClearValue sceneClearValues[3] = {};
    sceneClearValues[0].color.float32[0] = 0.02f;
    sceneClearValues[0].color.float32[1] = 0.024f;
    sceneClearValues[0].color.float32[2] = 0.032f;
    sceneClearValues[0].color.float32[3] = 1.0f;
    // 背景的法线取朝向相机的方向，否则背景之间会被判成互相之间都是边缘
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

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.vertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);

    if (hullMode) {
        // 先把外扩出来的一层画出来，正面剔除之后只留下包在物体外面的背面
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.hullPipeline);
        vkCmdDrawIndexed(frame.commandBuffer, renderer.boxIndexCount, renderer.boxInstanceCount, 0, 0, 0);
        ++drawCallCount;
        vkCmdDrawIndexed(frame.commandBuffer, renderer.plateIndexCount, renderer.plateInstanceCount,
                         renderer.plateIndexOffset, 0, renderer.boxInstanceCount);
        ++drawCallCount;
    }

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.sceneBoxPipeline);
    vkCmdDrawIndexed(frame.commandBuffer, renderer.boxIndexCount, renderer.boxInstanceCount, 0, 0, 0);
    ++drawCallCount;

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.scenePlatePipeline);
    vkCmdDrawIndexed(frame.commandBuffer, renderer.plateIndexCount, renderer.plateInstanceCount,
                     renderer.plateIndexOffset, 0, renderer.boxInstanceCount);
    ++drawCallCount;

    vkCmdEndRenderPass(frame.commandBuffer);

    outStatistics.cpuRecordSceneMilliseconds = (nowSeconds() - sceneStart) * 1000.0;

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
                            renderer.compositePipelineLayout, 0, 1, &frame.sceneSet, 0, nullptr);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      input.options.mode == OUTLINE_POST ? renderer.outlinePipeline
                                                         : renderer.presentPipeline);
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
