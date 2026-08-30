#include "renderer.h"

#include "vk_check.h"

#include <GLFW/glfw3.h>

#include <cstddef>
#include <cstring>

static const VkFormat GBUFFER_ALBEDO_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat GBUFFER_NORMAL_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat GBUFFER_POSITION_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkFormat GBUFFER_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

static void createGBufferRenderPass(const VulkanContext& ctx, Renderer& renderer)
{
    VkAttachmentDescription attachments[4] = {};
    const VkFormat colorFormats[3] = { GBUFFER_ALBEDO_FORMAT, GBUFFER_NORMAL_FORMAT, GBUFFER_POSITION_FORMAT };

    for (int i = 0; i < 3; ++i) {
        attachments[i].format = colorFormats[i];
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    attachments[3].format = GBUFFER_DEPTH_FORMAT;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRefs[3] = {};
    for (int i = 0; i < 3; ++i) {
        colorRefs[i].attachment = static_cast<uint32_t>(i);
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

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 4;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.gbufferRenderPass));
}

static void createLightingRenderPass(const VulkanContext& ctx, Renderer& renderer)
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = ctx.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.lightingRenderPass));
}

static void createGBufferTargets(const VulkanContext& ctx, Renderer& renderer)
{
    const uint32_t width = ctx.swapchainExtent.width;
    const uint32_t height = ctx.swapchainExtent.height;
    const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    createAttachmentTexture(ctx, width, height, GBUFFER_ALBEDO_FORMAT, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                            renderer.gbuffer.albedoOcclusion);
    createAttachmentTexture(ctx, width, height, GBUFFER_NORMAL_FORMAT, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                            renderer.gbuffer.normalRoughness);
    createAttachmentTexture(ctx, width, height, GBUFFER_POSITION_FORMAT, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                            renderer.gbuffer.positionMetallic);
    createAttachmentTexture(ctx, width, height, GBUFFER_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, renderer.gbuffer.depth);

    VkImageView views[4] = { renderer.gbuffer.albedoOcclusion.view, renderer.gbuffer.normalRoughness.view,
                             renderer.gbuffer.positionMetallic.view, renderer.gbuffer.depth.view };

    VkFramebufferCreateInfo framebufferInfo = {};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderer.gbufferRenderPass;
    framebufferInfo.attachmentCount = 4;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.gbuffer.framebuffer));
}

static void createDescriptorLayouts(const VulkanContext& ctx, Renderer& renderer)
{
    VkDescriptorSetLayoutBinding sceneBindings[2] = {};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                  VK_SHADER_STAGE_COMPUTE_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[1].descriptorCount = 1;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo sceneLayoutInfo = {};
    sceneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sceneLayoutInfo.bindingCount = 2;
    sceneLayoutInfo.pBindings = sceneBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &sceneLayoutInfo, nullptr, &renderer.sceneSetLayout));

    VkDescriptorSetLayoutBinding visibleBinding = {};
    visibleBinding.binding = 0;
    visibleBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    visibleBinding.descriptorCount = 1;
    visibleBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo visibleLayoutInfo = {};
    visibleLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    visibleLayoutInfo.bindingCount = 1;
    visibleLayoutInfo.pBindings = &visibleBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &visibleLayoutInfo, nullptr, &renderer.visibleSetLayout));

    VkDescriptorSetLayoutBinding materialBindings[5] = {};
    for (int i = 0; i < 5; ++i) {
        materialBindings[i].binding = static_cast<uint32_t>(i);
        materialBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialBindings[i].descriptorCount = 1;
        materialBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo materialLayoutInfo = {};
    materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    materialLayoutInfo.bindingCount = 5;
    materialLayoutInfo.pBindings = materialBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &materialLayoutInfo, nullptr, &renderer.materialSetLayout));

    VkDescriptorSetLayoutBinding lightingBindings[4] = {};
    for (int i = 0; i < 3; ++i) {
        lightingBindings[i].binding = static_cast<uint32_t>(i);
        lightingBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lightingBindings[i].descriptorCount = 1;
        lightingBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    lightingBindings[3].binding = 3;
    lightingBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lightingBindings[3].descriptorCount = 1;
    lightingBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lightingLayoutInfo = {};
    lightingLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lightingLayoutInfo.bindingCount = 4;
    lightingLayoutInfo.pBindings = lightingBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &lightingLayoutInfo, nullptr, &renderer.lightingSetLayout));

    VkDescriptorSetLayoutBinding cullBindings[2] = {};
    for (int i = 0; i < 2; ++i) {
        cullBindings[i].binding = static_cast<uint32_t>(i);
        cullBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cullBindings[i].descriptorCount = 1;
        cullBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo cullLayoutInfo = {};
    cullLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    cullLayoutInfo.bindingCount = 2;
    cullLayoutInfo.pBindings = cullBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &cullLayoutInfo, nullptr, &renderer.cullSetLayout));
}

static void createDescriptorPool(const VulkanContext& ctx, Renderer& renderer)
{
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
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

static VkDescriptorSet allocateDescriptorSet(const VulkanContext& ctx, Renderer& renderer,
                                             VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = renderer.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device, &allocInfo, &set));
    return set;
}

static void writeBufferDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding,
                                  VkDescriptorType type, const GpuBuffer& buffer)
{
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = buffer.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = buffer.size;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}

static void writeImageDescriptor(const VulkanContext& ctx, VkDescriptorSet set, uint32_t binding,
                                 VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
}

static void createGBufferPipeline(const VulkanContext& ctx, Renderer& renderer)
{
    VkDescriptorSetLayout setLayouts[3] = { renderer.sceneSetLayout, renderer.visibleSetLayout,
                                            renderer.materialSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.gbufferPipelineLayout));

    VkShaderModule vertexModule = loadShaderModule(ctx, std::string(SHADER_BINARY_DIR) + "/gbuffer.vert.spv");
    VkShaderModule fragmentModule = loadShaderModule(ctx, std::string(SHADER_BINARY_DIR) + "/gbuffer.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertexBinding = {};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(MeshVertex);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertexAttributes[3] = {};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertexAttributes[0].offset = static_cast<uint32_t>(offsetof(MeshVertex, position));
    vertexAttributes[1].location = 1;
    vertexAttributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertexAttributes[1].offset = static_cast<uint32_t>(offsetof(MeshVertex, normal));
    vertexAttributes[2].location = 2;
    vertexAttributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    vertexAttributes[2].offset = static_cast<uint32_t>(offsetof(MeshVertex, uv));

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

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachments[3] = {};
    for (int i = 0; i < 3; ++i) {
        blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 3;
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
    pipelineInfo.layout = renderer.gbufferPipelineLayout;
    pipelineInfo.renderPass = renderer.gbufferRenderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &renderer.gbufferPipeline));

    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
}

static void createLightingPipeline(const VulkanContext& ctx, Renderer& renderer)
{
    VkDescriptorSetLayout setLayouts[2] = { renderer.sceneSetLayout, renderer.lightingSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.lightingPipelineLayout));

    VkShaderModule vertexModule = loadShaderModule(ctx, std::string(SHADER_BINARY_DIR) + "/fullscreen.vert.spv");
    VkShaderModule fragmentModule = loadShaderModule(ctx, std::string(SHADER_BINARY_DIR) + "/lighting.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

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
    pipelineInfo.layout = renderer.lightingPipelineLayout;
    pipelineInfo.renderPass = renderer.lightingRenderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &renderer.lightingPipeline));

    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
    vkDestroyShaderModule(ctx.device, fragmentModule, nullptr);
}

static void createCullPipeline(const VulkanContext& ctx, Renderer& renderer)
{
    VkDescriptorSetLayout setLayouts[2] = { renderer.sceneSetLayout, renderer.cullSetLayout };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &renderer.cullPipelineLayout));

    VkShaderModule computeModule = loadShaderModule(ctx, std::string(SHADER_BINARY_DIR) + "/cull.comp.spv");

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = computeModule;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = renderer.cullPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                      &renderer.cullPipeline));

    vkDestroyShaderModule(ctx.device, computeModule, nullptr);
}

void createRenderer(const VulkanContext& ctx, Renderer& renderer, const MeshData& mesh,
                    const std::vector<InstanceData>& instances, uint32_t lightCount)
{
    renderer = Renderer();
    renderer.indexCount = static_cast<uint32_t>(mesh.indices.size());
    renderer.boundsRadius = mesh.boundsRadius;
    renderer.instanceCount = static_cast<uint32_t>(instances.size());
    renderer.lightCount = lightCount;

    const VkDeviceSize vertexBytes = sizeof(MeshVertex) * mesh.vertices.size();
    createBuffer(ctx, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.vertexBuffer);
    uploadBufferData(ctx, renderer.vertexBuffer, mesh.vertices.data(), vertexBytes);

    const VkDeviceSize indexBytes = sizeof(uint32_t) * mesh.indices.size();
    createBuffer(ctx, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.indexBuffer);
    uploadBufferData(ctx, renderer.indexBuffer, mesh.indices.data(), indexBytes);

    const VkDeviceSize instanceBytes = sizeof(InstanceData) * instances.size();
    createBuffer(ctx, instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, renderer.instanceBuffer);
    uploadBufferData(ctx, renderer.instanceBuffer, instances.data(), instanceBytes);

    const std::string assetDirectory = std::string(PROJECT_ROOT_DIR) + "/assets/backpack/";
    createTextureFromFile(ctx, assetDirectory + "diffuse.jpg", true, renderer.material.albedo);
    createTextureFromFile(ctx, assetDirectory + "normal.png", false, renderer.material.normal);
    createTextureFromFile(ctx, assetDirectory + "specular.jpg", false, renderer.material.metallic);
    createTextureFromFile(ctx, assetDirectory + "roughness.jpg", false, renderer.material.roughness);
    createTextureFromFile(ctx, assetDirectory + "ao.jpg", false, renderer.material.ambientOcclusion);
    renderer.material.sampler = createLinearSampler(ctx, renderer.material.albedo.mipLevels);

    createGBufferRenderPass(ctx, renderer);
    createLightingRenderPass(ctx, renderer);
    createGBufferTargets(ctx, renderer);

    renderer.presentFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.resize(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderer.lightingRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &ctx.swapchainImageViews[i];
        framebufferInfo.width = ctx.swapchainExtent.width;
        framebufferInfo.height = ctx.swapchainExtent.height;
        framebufferInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &renderer.presentFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }

    createDescriptorLayouts(ctx, renderer);
    createDescriptorPool(ctx, renderer);
    createGBufferPipeline(ctx, renderer);
    createLightingPipeline(ctx, renderer);
    createCullPipeline(ctx, renderer);

    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    renderer.materialSet = allocateDescriptorSet(ctx, renderer, renderer.materialSetLayout);
    writeImageDescriptor(ctx, renderer.materialSet, 0, renderer.material.albedo.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 1, renderer.material.normal.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 2, renderer.material.metallic.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 3, renderer.material.roughness.view, renderer.material.sampler);
    writeImageDescriptor(ctx, renderer.materialSet, 4, renderer.material.ambientOcclusion.view,
                         renderer.material.sampler);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        FrameResources& frame = renderer.frames[i];

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

        createBuffer(ctx, sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.cameraBuffer);
        createBuffer(ctx, sizeof(LightData) * renderer.lightCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.lightBuffer);
        createBuffer(ctx, sizeof(uint32_t) * renderer.instanceCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.cpuVisibleBuffer);
        createBuffer(ctx, sizeof(uint32_t) * renderer.instanceCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.gpuVisibleBuffer);
        createBuffer(ctx, sizeof(VkDrawIndexedIndirectCommand),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.indirectBuffer);
        createBuffer(ctx, sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     frame.visibleCountReadbackBuffer);

        // 绘制命令中除实例数量以外的字段保持不变，只需初始化一次
        VkDrawIndexedIndirectCommand initialCommand = {};
        initialCommand.indexCount = renderer.indexCount;
        initialCommand.instanceCount = 0;
        initialCommand.firstIndex = 0;
        initialCommand.vertexOffset = 0;
        initialCommand.firstInstance = 0;
        uploadBufferData(ctx, frame.indirectBuffer, &initialCommand, sizeof(initialCommand));

        frame.sceneSet = allocateDescriptorSet(ctx, renderer, renderer.sceneSetLayout);
        writeBufferDescriptor(ctx, frame.sceneSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame.cameraBuffer);
        writeBufferDescriptor(ctx, frame.sceneSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, renderer.instanceBuffer);

        frame.cpuVisibleSet = allocateDescriptorSet(ctx, renderer, renderer.visibleSetLayout);
        writeBufferDescriptor(ctx, frame.cpuVisibleSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              frame.cpuVisibleBuffer);

        frame.gpuVisibleSet = allocateDescriptorSet(ctx, renderer, renderer.visibleSetLayout);
        writeBufferDescriptor(ctx, frame.gpuVisibleSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              frame.gpuVisibleBuffer);

        frame.cullSet = allocateDescriptorSet(ctx, renderer, renderer.cullSetLayout);
        writeBufferDescriptor(ctx, frame.cullSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.gpuVisibleBuffer);
        writeBufferDescriptor(ctx, frame.cullSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.indirectBuffer);

        frame.lightingSet = allocateDescriptorSet(ctx, renderer, renderer.lightingSetLayout);
        writeImageDescriptor(ctx, frame.lightingSet, 0, renderer.gbuffer.albedoOcclusion.view,
                             renderer.material.sampler);
        writeImageDescriptor(ctx, frame.lightingSet, 1, renderer.gbuffer.normalRoughness.view,
                             renderer.material.sampler);
        writeImageDescriptor(ctx, frame.lightingSet, 2, renderer.gbuffer.positionMetallic.view,
                             renderer.material.sampler);
        writeBufferDescriptor(ctx, frame.lightingSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame.lightBuffer);

        VkQueryPoolCreateInfo queryPoolInfo = {};
        queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryPoolInfo.queryCount = 2;
        VK_CHECK(vkCreateQueryPool(ctx.device, &queryPoolInfo, nullptr, &frame.timestampPool));
        frame.timestampsValid = false;
    }
}

void destroyRenderer(const VulkanContext& ctx, Renderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        FrameResources& frame = renderer.frames[i];
        vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        destroyBuffer(ctx, frame.visibleCountReadbackBuffer);
        destroyBuffer(ctx, frame.indirectBuffer);
        destroyBuffer(ctx, frame.gpuVisibleBuffer);
        destroyBuffer(ctx, frame.cpuVisibleBuffer);
        destroyBuffer(ctx, frame.lightBuffer);
        destroyBuffer(ctx, frame.cameraBuffer);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.commandBuffer);
    }

    vkDestroyPipeline(ctx.device, renderer.cullPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.cullPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.lightingPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.lightingPipelineLayout, nullptr);
    vkDestroyPipeline(ctx.device, renderer.gbufferPipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.gbufferPipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.cullSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.lightingSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.materialSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.visibleSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sceneSetLayout, nullptr);

    for (uint32_t i = 0; i < renderer.presentFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.presentFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }

    vkDestroyFramebuffer(ctx.device, renderer.gbuffer.framebuffer, nullptr);
    destroyTexture(ctx, renderer.gbuffer.depth);
    destroyTexture(ctx, renderer.gbuffer.positionMetallic);
    destroyTexture(ctx, renderer.gbuffer.normalRoughness);
    destroyTexture(ctx, renderer.gbuffer.albedoOcclusion);

    vkDestroyRenderPass(ctx.device, renderer.lightingRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.gbufferRenderPass, nullptr);

    vkDestroySampler(ctx.device, renderer.material.sampler, nullptr);
    destroyTexture(ctx, renderer.material.ambientOcclusion);
    destroyTexture(ctx, renderer.material.roughness);
    destroyTexture(ctx, renderer.material.metallic);
    destroyTexture(ctx, renderer.material.normal);
    destroyTexture(ctx, renderer.material.albedo);

    destroyBuffer(ctx, renderer.instanceBuffer);
    destroyBuffer(ctx, renderer.indexBuffer);
    destroyBuffer(ctx, renderer.vertexBuffer);
}

void drawFrame(const VulkanContext& ctx, Renderer& renderer, uint64_t frameCounter, DrawPath drawPath,
               const CameraUniform& cameraUniform, const std::vector<LightData>& lights,
               const std::vector<InstanceData>& instances, uint32_t* visibleIndices, float boundsRadius,
               const GpuBuffer* captureBuffer, FrameStatistics& outStatistics)
{
    FrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    // 上一次使用本组资源的那一帧已经完成，可以读取它的计时与可见数量
    outStatistics.gpuMilliseconds = 0.0;
    if (frame.timestampsValid) {
        uint64_t timestamps[2] = { 0, 0 };
        VK_CHECK(vkGetQueryPoolResults(ctx.device, frame.timestampPool, 0, 2, sizeof(timestamps), timestamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        outStatistics.gpuMilliseconds = static_cast<double>(timestamps[1] - timestamps[0]) *
                                        static_cast<double>(renderer.timestampPeriodNanoseconds) / 1000000.0;
    }

    uint32_t imageIndex = 0;
    VK_CHECK(vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE,
                                   &imageIndex));

    if (renderer.imageFences[imageIndex] != VK_NULL_HANDLE) {
        VK_CHECK(vkWaitForFences(ctx.device, 1, &renderer.imageFences[imageIndex], VK_TRUE, UINT64_MAX));
    }
    renderer.imageFences[imageIndex] = frame.inFlight;
    VK_CHECK(vkResetFences(ctx.device, 1, &frame.inFlight));

    std::memcpy(frame.cameraBuffer.mapped, &cameraUniform, sizeof(CameraUniform));
    std::memcpy(frame.lightBuffer.mapped, lights.data(), sizeof(LightData) * lights.size());

    // 传统路径的剔除在主机上完成，可见列表逐帧写入主机可见内存
    uint32_t cpuVisibleCount = 0;
    outStatistics.cpuCullMilliseconds = 0.0;
    if (drawPath == DRAW_PATH_TRADITIONAL) {
        const double cullStart = glfwGetTime();
        cpuVisibleCount = cullInstancesOnCpu(instances, cameraUniform.frustumPlanes, boundsRadius, visibleIndices);
        std::memcpy(frame.cpuVisibleBuffer.mapped, visibleIndices, sizeof(uint32_t) * cpuVisibleCount);
        outStatistics.cpuCullMilliseconds = (glfwGetTime() - cullStart) * 1000.0;
    } else {
        // indirect 路径读取上一轮同组资源写回的可见数量，仅用于显示
        cpuVisibleCount = *static_cast<const uint32_t*>(frame.visibleCountReadbackBuffer.mapped);
    }

    const double recordStart = glfwGetTime();

    VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    vkCmdResetQueryPool(frame.commandBuffer, frame.timestampPool, 0, 2);
    vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool, 0);

    if (drawPath == DRAW_PATH_INDIRECT) {
        // 实例数量清零后由计算着色器用原子累加填充
        vkCmdFillBuffer(frame.commandBuffer, frame.indirectBuffer.buffer,
                        offsetof(VkDrawIndexedIndirectCommand, instanceCount), sizeof(uint32_t), 0);

        VkBufferMemoryBarrier clearBarrier = {};
        clearBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.buffer = frame.indirectBuffer.buffer;
        clearBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &clearBarrier, 0, nullptr);

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.cullPipeline);
        VkDescriptorSet cullSets[2] = { frame.sceneSet, frame.cullSet };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.cullPipelineLayout,
                                0, 2, cullSets, 0, nullptr);
        vkCmdDispatch(frame.commandBuffer, (renderer.instanceCount + 63) / 64, 1, 1);

        VkBufferMemoryBarrier cullBarriers[2] = {};
        cullBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        cullBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cullBarriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        cullBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cullBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cullBarriers[0].buffer = frame.indirectBuffer.buffer;
        cullBarriers[0].size = VK_WHOLE_SIZE;

        cullBarriers[1] = cullBarriers[0];
        cullBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        cullBarriers[1].buffer = frame.gpuVisibleBuffer.buffer;

        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 0, nullptr, 2, cullBarriers, 0, nullptr);

        // 把实例数量取回主机，仅用于界面显示
        VkBufferCopy countCopy = {};
        countCopy.srcOffset = offsetof(VkDrawIndexedIndirectCommand, instanceCount);
        countCopy.size = sizeof(uint32_t);
        vkCmdCopyBuffer(frame.commandBuffer, frame.indirectBuffer.buffer,
                        frame.visibleCountReadbackBuffer.buffer, 1, &countCopy);
    }

    VkViewport viewport = {};
    viewport.width = static_cast<float>(ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(ctx.swapchainExtent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.extent = ctx.swapchainExtent;

    VkClearValue gbufferClearValues[4] = {};
    gbufferClearValues[3].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo gbufferBegin = {};
    gbufferBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    gbufferBegin.renderPass = renderer.gbufferRenderPass;
    gbufferBegin.framebuffer = renderer.gbuffer.framebuffer;
    gbufferBegin.renderArea.extent = ctx.swapchainExtent;
    gbufferBegin.clearValueCount = 4;
    gbufferBegin.pClearValues = gbufferClearValues;

    vkCmdBeginRenderPass(frame.commandBuffer, &gbufferBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gbufferPipeline);

    VkDescriptorSet gbufferSets[3] = { frame.sceneSet,
                                       drawPath == DRAW_PATH_TRADITIONAL ? frame.cpuVisibleSet
                                                                         : frame.gpuVisibleSet,
                                       renderer.materialSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gbufferPipelineLayout,
                            0, 3, gbufferSets, 0, nullptr);

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &renderer.vertexBuffer.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, renderer.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    if (drawPath == DRAW_PATH_TRADITIONAL) {
        // 每个可见实例记录一条绘制命令
        for (uint32_t i = 0; i < cpuVisibleCount; ++i) {
            vkCmdDrawIndexed(frame.commandBuffer, renderer.indexCount, 1, 0, 0, i);
        }
        outStatistics.drawCallCount = cpuVisibleCount;
    } else {
        // 实例数量由显存中的绘制命令决定，主机不需要知道可见集合
        vkCmdDrawIndexedIndirect(frame.commandBuffer, frame.indirectBuffer.buffer, 0, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
        outStatistics.drawCallCount = 1;
    }

    vkCmdEndRenderPass(frame.commandBuffer);

    VkRenderPassBeginInfo lightingBegin = {};
    lightingBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    lightingBegin.renderPass = renderer.lightingRenderPass;
    lightingBegin.framebuffer = renderer.presentFramebuffers[imageIndex];
    lightingBegin.renderArea.extent = ctx.swapchainExtent;

    vkCmdBeginRenderPass(frame.commandBuffer, &lightingBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.lightingPipeline);

    VkDescriptorSet lightingSets[2] = { frame.sceneSet, frame.lightingSet };
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.lightingPipelineLayout,
                            0, 2, lightingSets, 0, nullptr);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(frame.commandBuffer);

    if (captureBuffer != nullptr) {
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
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer->buffer, 1, &copyRegion);

        VkImageMemoryBarrier backToPresent = toTransferSource;
        backToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        backToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        backToPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        backToPresent.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &backToPresent);
    }

    vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool, 1);

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

    frame.timestampsValid = true;
    outStatistics.cpuRecordMilliseconds = (glfwGetTime() - recordStart) * 1000.0;
    outStatistics.visibleInstanceCount = cpuVisibleCount;

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderer.presentSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &ctx.swapchain;
    presentInfo.pImageIndices = &imageIndex;
    VK_CHECK(vkQueuePresentKHR(ctx.queue, &presentInfo));
}
