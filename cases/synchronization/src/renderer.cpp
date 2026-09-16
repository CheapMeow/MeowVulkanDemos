#include "renderer.h"

#include "asset_file.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"

#include <cstring>
#include <vector>

// 图案纹理既要做计算着色器的存储图像，又要做光栅化生产者与消费者的颜色附件、
// 采样纹理与输入附件，因此四条用途都要声明。
// 格式取无符号整数：浮点到定点数的取整方式由实现决定，两条生产者路径写入的字节
// 只有在整数格式下才严格相同
static const VkFormat PATTERN_FORMAT = VK_FORMAT_R8G8B8A8_UINT;

const char* syncModeName(uint32_t mode)
{
    switch (mode) {
        case SYNC_MODE_BARRIER:
            return "barrier";
        case SYNC_MODE_EVENT:
            return "event";
        case SYNC_MODE_SEMAPHORE_BINARY:
            return "semaphore_binary";
        case SYNC_MODE_SEMAPHORE_TIMELINE:
            return "semaphore_timeline";
        case SYNC_MODE_FENCE:
            return "fence";
        case SYNC_MODE_QUEUE_IDLE:
            return "queue_idle";
        case SYNC_MODE_DEVICE_IDLE:
            return "device_idle";
        case SYNC_MODE_SUBPASS:
            return "subpass";
        default:
            break;
    }
    return "unknown";
}

bool parseSyncMode(const char* name, uint32_t& outMode)
{
    for (uint32_t mode = 0; mode < SYNC_MODE_COUNT; ++mode) {
        if (std::strcmp(name, syncModeName(mode)) == 0) {
            outMode = mode;
            return true;
        }
    }
    return false;
}

uint32_t syncModeRenderPassFamily(uint32_t mode)
{
    return mode == SYNC_MODE_SUBPASS ? 1u : 0u;
}

void syncModeMaskSummary(uint32_t mode, const char*& outFirstLine, const char*& outSecondLine)
{
    switch (mode) {
        case SYNC_MODE_BARRIER:
            outFirstLine = "vkCmdPipelineBarrier: srcStageMask=COMPUTE_SHADER srcAccessMask=SHADER_WRITE";
            outSecondLine = "                      dstStageMask=FRAGMENT_SHADER dstAccessMask=SHADER_READ";
            return;
        case SYNC_MODE_EVENT:
            outFirstLine = "vkCmdSetEvent: stageMask=COMPUTE_SHADER（只定义执行依赖）";
            outSecondLine = "vkCmdWaitEvents: srcStage=COMPUTE_SHADER srcAccess=SHADER_WRITE "
                            "dstStage=FRAGMENT_SHADER dstAccess=SHADER_READ";
            return;
        case SYNC_MODE_SEMAPHORE_BINARY:
        case SYNC_MODE_SEMAPHORE_TIMELINE:
            outFirstLine = "生产者提交: 末尾屏障 dstStage=ALL_COMMANDS dstAccess=0 只做转布局与置为可用";
            outSecondLine = "消费者提交: 等待信号量，pWaitDstStageMask=FRAGMENT_SHADER";
            return;
        case SYNC_MODE_FENCE:
        case SYNC_MODE_QUEUE_IDLE:
        case SYNC_MODE_DEVICE_IDLE:
            outFirstLine = "生产者提交: 末尾屏障 dstStage=ALL_COMMANDS dstAccess=0 只做转布局与置为可用";
            outSecondLine = "主机等待生产者完成后才提交消费者，没有掩码可调";
            return;
        case SYNC_MODE_SUBPASS:
            outFirstLine = "子通道依赖 0->1: srcStage=COLOR_ATTACHMENT_OUTPUT srcAccess=COLOR_ATTACHMENT_WRITE";
            outSecondLine = "                dstStage=FRAGMENT_SHADER dstAccess=INPUT_ATTACHMENT_READ";
            return;
        default:
            break;
    }
    outFirstLine = "";
    outSecondLine = "";
}

static VkImageMemoryBarrier makePatternBarrier(const SynchronizationRenderer& renderer,
                                               VkImageLayout oldLayout, VkImageLayout newLayout,
                                               VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = renderer.patternTexture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

// 图案在上一帧被谁碰过取决于上一帧用的是哪种同步方式：计算着色器写、子通道 0 的光栅化写、
// 采样读或输入附件读。所以写之前这一侧一律覆盖全部命令，读之前那一侧才写成具体的阶段
static VkPipelineStageFlags PATTERN_PREVIOUS_ACCESS_STAGES = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
static const VkAccessFlags PATTERN_PREVIOUS_ACCESS_MASK =
    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

// 生产者开始前把图案转到通用布局。读后写只需要执行依赖，但布局转换本身会读写内存，
// 因此把上一帧的写入与读取都写进可用性与执行依赖里
static void recordPatternAcquire(VkCommandBuffer commandBuffer, const SynchronizationRenderer& renderer)
{
    VkImageMemoryBarrier barrier =
        makePatternBarrier(renderer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                           PATTERN_PREVIOUS_ACCESS_MASK, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(commandBuffer, PATTERN_PREVIOUS_ACCESS_STAGES, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

static void recordRenderPassClear(const SynchronizationRenderer& renderer, bool subpassRenderPass,
                                  VkRenderPassBeginInfo& outBegin, VkClearValue (&clearValues)[2])
{
    clearValues[0].color.float32[0] = 0.02f;
    clearValues[0].color.float32[1] = 0.025f;
    clearValues[0].color.float32[2] = 0.035f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].color.float32[0] = 0.0f;
    clearValues[1].color.float32[1] = 0.0f;
    clearValues[1].color.float32[2] = 0.0f;
    clearValues[1].color.float32[3] = 1.0f;

    outBegin = VkRenderPassBeginInfo();
    outBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    outBegin.renderPass = subpassRenderPass ? renderer.subpassRenderPass : renderer.consumerRenderPass;
    outBegin.clearValueCount = subpassRenderPass ? 2 : 1;
    outBegin.pClearValues = clearValues;
}

static void recordCaptureCopy(const VulkanContext& ctx, VkCommandBuffer commandBuffer, uint32_t imageIndex,
                              const GpuBuffer& captureBuffer)
{
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
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransferSource);

    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent.width = ctx.swapchainExtent.width;
    copyRegion.imageExtent.height = ctx.swapchainExtent.height;
    copyRegion.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(commandBuffer, ctx.swapchainImages[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer.buffer, 1, &copyRegion);

    VkImageMemoryBarrier backToPresent = toTransferSource;
    backToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    backToPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToPresent.dstAccessMask = 0;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &backToPresent);
}

static void createConsumerRenderPass(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    VkAttachmentDescription attachment = {};
    attachment.format = ctx.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.consumerRenderPass));
}

// 图案在子通道 0 里作为颜色附件被写出，在子通道 1 里作为输入附件被读取。上一帧的用法与
// 这一帧之间只有位置相同的子通道才有隐式依赖，跨子通道必须显式给出
static void createSubpassRenderPass(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = PATTERN_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    // 每个像素都会被子通道 0 的全屏三角形写满，载入与保留都没有意义
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = ctx.swapchainFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference patternColorRef = {};
    patternColorRef.attachment = 0;
    patternColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference patternInputRef = {};
    patternInputRef.attachment = 0;
    patternInputRef.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference presentColorRef = {};
    presentColorRef.attachment = 1;
    presentColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpasses[2] = {};
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = &patternColorRef;

    subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[1].inputAttachmentCount = 1;
    subpasses[1].pInputAttachments = &patternInputRef;
    subpasses[1].colorAttachmentCount = 1;
    subpasses[1].pColorAttachments = &presentColorRef;

    VkSubpassDependency dependencies[3] = {};
    // 进入渲染通道之前：上一帧对图案的读取与写入都要先完成，图案才能被重新写入
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = PATTERN_PREVIOUS_ACCESS_STAGES;
    dependencies[0].srcAccessMask = PATTERN_PREVIOUS_ACCESS_MASK;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // 子通道 0 写完图案，子通道 1 才能读
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

    // 离开渲染通道：图案的读取与交换链图像的写入都到此为止
    dependencies[2].srcSubpass = 1;
    dependencies[2].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[2].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[2].srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 2;
    renderPassInfo.pSubpasses = subpasses;
    renderPassInfo.dependencyCount = 3;
    renderPassInfo.pDependencies = dependencies;

    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderer.subpassRenderPass));
}

// 图案纹理的初始内容是未定义的，先清成固定颜色再进只读布局，之后每一帧都从只读布局开始
static void createPatternTexture(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    createAttachmentTexture(ctx, ctx.swapchainExtent.width, ctx.swapchainExtent.height, PATTERN_FORMAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, renderer.patternTexture);

    VkCommandBuffer commandBuffer = beginOneTimeCommands(ctx);

    VkImageMemoryBarrier toClear = makePatternBarrier(renderer, VK_IMAGE_LAYOUT_UNDEFINED,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                                      VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toClear);

    VkClearColorValue clearColor = {};
    clearColor.uint32[3] = 255;
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(commandBuffer, renderer.patternTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &range);

    VkImageMemoryBarrier toReadOnly = makePatternBarrier(
        renderer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toReadOnly);

    endOneTimeCommands(ctx, commandBuffer);
}

static void createSwapchainTargets(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    createPatternTexture(ctx, renderer);

    renderer.consumerFramebuffers.resize(ctx.swapchainImageCount);
    renderer.subpassFramebuffers.resize(ctx.swapchainImageCount);
    renderer.presentSemaphores.resize(ctx.swapchainImageCount);
    renderer.imageFences.assign(ctx.swapchainImageCount, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkImageView consumerViews[1] = { ctx.swapchainImageViews[i] };
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderer.consumerRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = consumerViews;
        framebufferInfo.width = ctx.swapchainExtent.width;
        framebufferInfo.height = ctx.swapchainExtent.height;
        framebufferInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr,
                                     &renderer.consumerFramebuffers[i]));

        VkImageView subpassViews[2] = { renderer.patternTexture.view, ctx.swapchainImageViews[i] };
        framebufferInfo.renderPass = renderer.subpassRenderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = subpassViews;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr,
                                     &renderer.subpassFramebuffers[i]));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &renderer.presentSemaphores[i]));
    }
}

static void destroySwapchainTargets(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    for (uint32_t i = 0; i < renderer.consumerFramebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, renderer.consumerFramebuffers[i], nullptr);
        vkDestroyFramebuffer(ctx.device, renderer.subpassFramebuffers[i], nullptr);
        vkDestroySemaphore(ctx.device, renderer.presentSemaphores[i], nullptr);
    }
    renderer.consumerFramebuffers.clear();
    renderer.subpassFramebuffers.clear();
    renderer.presentSemaphores.clear();
    renderer.imageFences.clear();
    destroyTexture(ctx, renderer.patternTexture);
}

static VkDescriptorSetLayoutBinding makeBinding(uint32_t binding, VkDescriptorType type,
                                                VkShaderStageFlags stageFlags)
{
    VkDescriptorSetLayoutBinding result = {};
    result.binding = binding;
    result.descriptorType = type;
    result.descriptorCount = 1;
    result.stageFlags = stageFlags;
    return result;
}

static VkDescriptorSetLayout createSetLayout(const VulkanContext& ctx,
                                             const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layoutInfo, nullptr, &layout));
    return layout;
}

static void createDescriptorLayouts(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    renderer.computeSetLayout = createSetLayout(
        ctx, { makeBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
               makeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT) });

    const VkShaderStageFlags graphicsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    renderer.rasterProducerSetLayout =
        createSetLayout(ctx, { makeBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, graphicsStages) });

    renderer.sampledConsumerSetLayout = createSetLayout(
        ctx, { makeBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, graphicsStages),
               makeBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT) });

    renderer.inputConsumerSetLayout = createSetLayout(
        ctx, { makeBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, graphicsStages),
               makeBinding(1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) });

    VkDescriptorPoolSize poolSizes[4] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 2;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 2;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    poolSizes[3].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &renderer.descriptorPool));
}

static VkPipelineLayout createPipelineLayout(const VulkanContext& ctx, VkDescriptorSetLayout setLayout)
{
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout));
    return layout;
}

// 三条图形管线只有渲染通道、子通道、管线布局与片元着色器不同，固定功能状态完全一样：
// 没有顶点输入，全屏三角形直接由 gl_VertexIndex 给出，不写深度，不混合
static VkPipeline createFullscreenPipeline(const VulkanContext& ctx, VkRenderPass renderPass, uint32_t subpass,
                                           VkPipelineLayout pipelineLayout, VkShaderModule vertexModule,
                                           VkShaderModule fragmentModule)
{
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

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = VK_FALSE;
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
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = subpass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    return pipeline;
}

static void createPipelines(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    VkShaderModule vertexModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/fullscreen.vert.spv"));
    VkShaderModule patternComputeModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/pattern.comp.spv"));
    VkShaderModule patternRasterModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/pattern.frag.spv"));
    VkShaderModule consumeModule = loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/consume.frag.spv"));
    VkShaderModule consumeInputModule =
        loadShaderModuleFromMemory(ctx, readAssetBytes(DEMO_SHADER_DIR "/consume_input.frag.spv"));

    renderer.computePipelineLayout = createPipelineLayout(ctx, renderer.computeSetLayout);
    renderer.rasterProducerPipelineLayout = createPipelineLayout(ctx, renderer.rasterProducerSetLayout);
    renderer.sampledConsumerPipelineLayout = createPipelineLayout(ctx, renderer.sampledConsumerSetLayout);
    renderer.inputConsumerPipelineLayout = createPipelineLayout(ctx, renderer.inputConsumerSetLayout);

    VkComputePipelineCreateInfo computeInfo = {};
    computeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeInfo.stage.module = patternComputeModule;
    computeInfo.stage.pName = "main";
    computeInfo.layout = renderer.computePipelineLayout;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &computeInfo, nullptr,
                                      &renderer.computePipeline));

    renderer.rasterProducerPipeline =
        createFullscreenPipeline(ctx, renderer.subpassRenderPass, 0, renderer.rasterProducerPipelineLayout,
                                 vertexModule, patternRasterModule);
    renderer.sampledConsumerPipeline =
        createFullscreenPipeline(ctx, renderer.consumerRenderPass, 0, renderer.sampledConsumerPipelineLayout,
                                 vertexModule, consumeModule);
    renderer.inputConsumerPipeline =
        createFullscreenPipeline(ctx, renderer.subpassRenderPass, 1, renderer.inputConsumerPipelineLayout,
                                 vertexModule, consumeInputModule);

    vkDestroyShaderModule(ctx.device, consumeInputModule, nullptr);
    vkDestroyShaderModule(ctx.device, consumeModule, nullptr);
    vkDestroyShaderModule(ctx.device, patternRasterModule, nullptr);
    vkDestroyShaderModule(ctx.device, patternComputeModule, nullptr);
    vkDestroyShaderModule(ctx.device, vertexModule, nullptr);
}

// 参数缓冲只要主机可见。所在内存类型不带 HOST_COHERENT 时，主机的写入必须显式冲洗
// 才对设备可见，这一步就是规范里的内存域操作
static void createParameterBuffer(const VulkanContext& ctx, SyncFrameResources& frame)
{
    createBuffer(ctx, sizeof(PatternUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, frame.uniformBuffer);

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx.device, frame.uniformBuffer.buffer, &requirements);
    const uint32_t typeIndex =
        findMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    frame.uniformBufferCoherent =
        (ctx.memoryProperties.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    frame.uniformFlushPerformed = !frame.uniformBufferCoherent;
}

static void writeParameterBuffer(const VulkanContext& ctx, SyncFrameResources& frame, const PatternUniform& uniform)
{
    std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(PatternUniform));

    if (!frame.uniformBufferCoherent) {
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = frame.uniformBuffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        VK_CHECK(vkFlushMappedMemoryRanges(ctx.device, 1, &range));
    }
}

static void writeFrameDescriptors(const VulkanContext& ctx, SynchronizationRenderer& renderer,
                                  SyncFrameResources& frame)
{
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.buffer = frame.uniformBuffer.buffer;
    bufferInfo.range = sizeof(PatternUniform);

    VkDescriptorImageInfo storageImageInfo = {};
    storageImageInfo.imageView = renderer.patternTexture.view;
    storageImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampledImageInfo = {};
    sampledImageInfo.sampler = renderer.patternSampler;
    sampledImageInfo.imageView = renderer.patternTexture.view;
    sampledImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo inputImageInfo = {};
    inputImageInfo.imageView = renderer.patternTexture.view;
    inputImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[5] = {};
    uint32_t writeCount = 0;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = frame.computeSet;
    writes[writeCount].dstBinding = 0;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[writeCount].pBufferInfo = &bufferInfo;
    ++writeCount;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = frame.computeSet;
    writes[writeCount].dstBinding = 1;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &storageImageInfo;
    ++writeCount;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = frame.rasterProducerSet;
    writes[writeCount].dstBinding = 0;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[writeCount].pBufferInfo = &bufferInfo;
    ++writeCount;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = frame.sampledConsumerSet;
    writes[writeCount].dstBinding = 0;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[writeCount].pBufferInfo = &bufferInfo;
    ++writeCount;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = frame.sampledConsumerSet;
    writes[writeCount].dstBinding = 1;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[writeCount].pImageInfo = &sampledImageInfo;
    ++writeCount;

    vkUpdateDescriptorSets(ctx.device, writeCount, writes, 0, nullptr);

    VkWriteDescriptorSet inputWrites[2] = {};
    inputWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    inputWrites[0].dstSet = frame.inputConsumerSet;
    inputWrites[0].dstBinding = 0;
    inputWrites[0].descriptorCount = 1;
    inputWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    inputWrites[0].pBufferInfo = &bufferInfo;

    inputWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    inputWrites[1].dstSet = frame.inputConsumerSet;
    inputWrites[1].dstBinding = 1;
    inputWrites[1].descriptorCount = 1;
    inputWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    inputWrites[1].pImageInfo = &inputImageInfo;

    vkUpdateDescriptorSets(ctx.device, 2, inputWrites, 0, nullptr);
}

void createRenderer(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    renderer = SynchronizationRenderer();

    if (!ctx.timelineSemaphoreSupported) {
        FATAL("this case demonstrates timeline semaphores, which this device does not support");
    }

    VkFormatProperties patternFormatProperties = {};
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, PATTERN_FORMAT, &patternFormatProperties);
    const VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((patternFormatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
        FATAL("the format used for the pattern image lacks a required feature");
    }

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &queueFamilyCount, queueFamilies.data());
    renderer.timestampsSupported =
        ctx.physicalDeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE &&
        queueFamilies[ctx.queueFamilyIndex].timestampValidBits != 0;
    renderer.timestampPeriodNanoseconds = ctx.physicalDeviceProperties.limits.timestampPeriod;

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // 最近邻取样，取样点落在像素中心时取到的就是同一个像素
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &renderer.patternSampler));

    createConsumerRenderPass(ctx, renderer);
    createSubpassRenderPass(ctx, renderer);
    createSwapchainTargets(ctx, renderer);
    createDescriptorLayouts(ctx, renderer);
    createPipelines(ctx, renderer);

    // 时间线信号量带一个 64 位计数值，全部帧共用一条，每帧递增
    VkSemaphoreTypeCreateInfo timelineType = {};
    timelineType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;

    VkSemaphoreCreateInfo timelineInfo = {};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineInfo.pNext = &timelineType;
    VK_CHECK(vkCreateSemaphore(ctx.device, &timelineInfo, nullptr, &renderer.patternReadyTimeline));

    renderer.getSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
        vkGetDeviceProcAddr(ctx.device, "vkGetSemaphoreCounterValue"));
    if (renderer.getSemaphoreCounterValue == nullptr) {
        renderer.getSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
            vkGetDeviceProcAddr(ctx.device, "vkGetSemaphoreCounterValueKHR"));
    }
    if (renderer.getSemaphoreCounterValue == nullptr) {
        FATAL("neither vkGetSemaphoreCounterValue nor vkGetSemaphoreCounterValueKHR is available");
    }

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SyncFrameResources& frame = renderer.frames[i];

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = ctx.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &frame.producerCommandBuffer));
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &frame.consumerCommandBuffer));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &frame.imageAvailable));
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &frame.patternReadyBinary));

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(ctx.device, &fenceInfo, nullptr, &frame.inFlight));

        fenceInfo.flags = 0;
        VK_CHECK(vkCreateFence(ctx.device, &fenceInfo, nullptr, &frame.patternSync));

        VkEventCreateInfo eventInfo = {};
        eventInfo.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        VK_CHECK(vkCreateEvent(ctx.device, &eventInfo, nullptr, &frame.patternEvent));

        createParameterBuffer(ctx, frame);

        frame.computeSet = allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.computeSetLayout);
        frame.rasterProducerSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.rasterProducerSetLayout);
        frame.sampledConsumerSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.sampledConsumerSetLayout);
        frame.inputConsumerSet =
            allocateDescriptorSet(ctx, renderer.descriptorPool, renderer.inputConsumerSetLayout);
        writeFrameDescriptors(ctx, renderer, frame);

        frame.timestampsValid = false;
        if (renderer.timestampsSupported) {
            VkQueryPoolCreateInfo queryPoolInfo = {};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            // 0、1 号夹住生产者那一段，2、3 号夹住消费者那一段
            queryPoolInfo.queryCount = 4;
            VK_CHECK(vkCreateQueryPool(ctx.device, &queryPoolInfo, nullptr, &frame.timestampPool));
        }
    }

    std::printf("pattern parameter buffer memory type: %s%s\n",
                renderer.frames[0].uniformBufferCoherent ? "host coherent" : "host visible but not coherent",
                renderer.frames[0].uniformBufferCoherent ? "" : ", flushed explicitly on every write");
    std::printf("timeline semaphore enabled, device timestamps %s\n",
                renderer.timestampsSupported ? "available" : "unavailable");
}

void destroyRenderer(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        SyncFrameResources& frame = renderer.frames[i];
        if (renderer.timestampsSupported) {
            vkDestroyQueryPool(ctx.device, frame.timestampPool, nullptr);
        }
        destroyBuffer(ctx, frame.uniformBuffer);
        vkDestroyEvent(ctx.device, frame.patternEvent, nullptr);
        vkDestroyFence(ctx.device, frame.patternSync, nullptr);
        vkDestroyFence(ctx.device, frame.inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frame.patternReadyBinary, nullptr);
        vkDestroySemaphore(ctx.device, frame.imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.consumerCommandBuffer);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frame.producerCommandBuffer);
    }

    vkDestroySemaphore(ctx.device, renderer.patternReadyTimeline, nullptr);

    vkDestroyPipeline(ctx.device, renderer.inputConsumerPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.sampledConsumerPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.rasterProducerPipeline, nullptr);
    vkDestroyPipeline(ctx.device, renderer.computePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.inputConsumerPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.sampledConsumerPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.rasterProducerPipelineLayout, nullptr);
    vkDestroyPipelineLayout(ctx.device, renderer.computePipelineLayout, nullptr);

    vkDestroyDescriptorPool(ctx.device, renderer.descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.inputConsumerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.sampledConsumerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.rasterProducerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, renderer.computeSetLayout, nullptr);

    destroySwapchainTargets(ctx, renderer);
    vkDestroyRenderPass(ctx.device, renderer.subpassRenderPass, nullptr);
    vkDestroyRenderPass(ctx.device, renderer.consumerRenderPass, nullptr);
    vkDestroySampler(ctx.device, renderer.patternSampler, nullptr);
}

void recreateSwapchainTargets(const VulkanContext& ctx, SynchronizationRenderer& renderer)
{
    destroySwapchainTargets(ctx, renderer);
    createSwapchainTargets(ctx, renderer);

    // 图案纹理换了，三个用到它的描述符集合都要重新写
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        writeFrameDescriptors(ctx, renderer, renderer.frames[i]);
    }
}

static void setFullscreenViewport(VkCommandBuffer commandBuffer, const VkExtent2D& extent)
{
    VkViewport viewport = {};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.extent = extent;

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

// 时间戳分成两段：生产者段的开头与结尾是 0、1 号查询，消费者段是 2、3 号查询。
// 验证层要求每条命令缓冲里写入的查询先在本命令缓冲里复位，因此两次提交的模式下
// 两条命令缓冲各复位自己要写的那一对
static void writeFrameTimestamp(const SynchronizationRenderer& renderer, const SyncFrameResources& frame,
                                VkCommandBuffer commandBuffer, uint32_t query, VkPipelineStageFlags stage)
{
    if (renderer.timestampsSupported) {
        vkCmdWriteTimestamp(commandBuffer, static_cast<VkPipelineStageFlagBits>(stage), frame.timestampPool,
                            query);
    }
}

static void resetFrameTimestamps(const SynchronizationRenderer& renderer, const SyncFrameResources& frame,
                                 VkCommandBuffer commandBuffer, uint32_t firstQuery, uint32_t queryCount)
{
    if (renderer.timestampsSupported) {
        vkCmdResetQueryPool(commandBuffer, frame.timestampPool, firstQuery, queryCount);
    }
}

// 生产者的计算部分：图案转到通用布局，派发计算着色器
static void recordComputeProducer(VkCommandBuffer commandBuffer, const SynchronizationRenderer& renderer,
                                  const SyncFrameResources& frame, const VkExtent2D& extent)
{
    recordPatternAcquire(commandBuffer, renderer);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.computePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.computePipelineLayout, 0, 1,
                            &frame.computeSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
}

// 生产者的收尾屏障：把这批写入变成可用，并把图案从通用布局转到只读布局。
// 单缓冲模式里它同时把结果交给片元着色器；两次提交的模式里第二侧只写 ALL_COMMANDS 与
// 空访问掩码，跨提交的那一半依赖由信号量、围栏或空闲等待给出
static void recordPatternRelease(VkCommandBuffer commandBuffer, const SynchronizationRenderer& renderer,
                                 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier =
        makePatternBarrier(renderer, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_SHADER_WRITE_BIT, dstAccess);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
}

// 消费者：采样图案并写交换链图像，随后是界面与可选的抓帧拷贝
static void recordSampledConsumer(VkCommandBuffer commandBuffer, const VulkanContext& ctx,
                                  const SynchronizationRenderer& renderer, const SyncFrameResources& frame,
                                  uint32_t imageIndex, const FrameInput& input, FrameStatistics& outStatistics)
{
    VkClearValue clearValues[2] = {};
    VkRenderPassBeginInfo renderPassBegin = {};
    recordRenderPassClear(renderer, false, renderPassBegin, clearValues);
    renderPassBegin.framebuffer = renderer.consumerFramebuffers[imageIndex];
    renderPassBegin.renderArea.extent = ctx.swapchainExtent;

    vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
    setFullscreenViewport(commandBuffer, ctx.swapchainExtent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.sampledConsumerPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.sampledConsumerPipelineLayout,
                            0, 1, &frame.sampledConsumerSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    ++outStatistics.drawCallCount;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    vkCmdEndRenderPass(commandBuffer);
}

// 子通道模式：一个渲染通道里先光栅化写图案，再按输入附件读图案并写交换链图像
static void recordSubpassConsumer(VkCommandBuffer commandBuffer, const VulkanContext& ctx,
                                  const SynchronizationRenderer& renderer, const SyncFrameResources& frame,
                                  uint32_t imageIndex, const FrameInput& input, FrameStatistics& outStatistics)
{
    VkClearValue clearValues[2] = {};
    VkRenderPassBeginInfo renderPassBegin = {};
    recordRenderPassClear(renderer, true, renderPassBegin, clearValues);
    renderPassBegin.framebuffer = renderer.subpassFramebuffers[imageIndex];
    renderPassBegin.renderArea.extent = ctx.swapchainExtent;

    vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);

    // 子通道 0：光栅化生产者
    const double producerStart = nowSeconds();
    setFullscreenViewport(commandBuffer, ctx.swapchainExtent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.rasterProducerPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.rasterProducerPipelineLayout,
                            0, 1, &frame.rasterProducerSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    ++outStatistics.drawCallCount;
    outStatistics.cpuRecordProducerMilliseconds = (nowSeconds() - producerStart) * 1000.0;

    vkCmdNextSubpass(commandBuffer, VK_SUBPASS_CONTENTS_INLINE);

    // 两个子通道的交界就在渲染通道内部，时间戳也只能记在这里
    writeFrameTimestamp(renderer, frame, commandBuffer, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    writeFrameTimestamp(renderer, frame, commandBuffer, 2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    // 子通道 1：按输入附件读图案的消费者
    setFullscreenViewport(commandBuffer, ctx.swapchainExtent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.inputConsumerPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.inputConsumerPipelineLayout,
                            0, 1, &frame.inputConsumerSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    ++outStatistics.drawCallCount;

    const double uiStart = nowSeconds();
    if (input.drawUserInterface) {
        recordUserInterfaceCommands(commandBuffer);
    }
    outStatistics.cpuRecordUiMilliseconds = (nowSeconds() - uiStart) * 1000.0;

    vkCmdEndRenderPass(commandBuffer);
}

static void beginCommandBuffer(VkCommandBuffer commandBuffer)
{
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
}

bool drawFrame(const VulkanContext& ctx, SynchronizationRenderer& renderer, uint64_t frameCounter,
               const FrameInput& input, const PatternUniform& uniform, FrameStatistics& outStatistics)
{
    outStatistics = FrameStatistics();

    SyncFrameResources& frame = renderer.frames[frameCounter % MAX_FRAMES_IN_FLIGHT];

    VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    // 上一次使用本组资源的那一帧已经完成，可以读它的计时
    outStatistics.gpuProducerMilliseconds = 0.0;
    outStatistics.gpuConsumerMilliseconds = 0.0;
    if (renderer.timestampsSupported && frame.timestampsValid) {
        uint64_t timestamps[4] = { 0, 0, 0, 0 };
        VK_CHECK(vkGetQueryPoolResults(ctx.device, frame.timestampPool, 0, 4, sizeof(timestamps), timestamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        const double nanosecondsToMilliseconds =
            static_cast<double>(renderer.timestampPeriodNanoseconds) / 1000000.0;
        outStatistics.gpuProducerMilliseconds =
            static_cast<double>(timestamps[1] - timestamps[0]) * nanosecondsToMilliseconds;
        outStatistics.gpuConsumerMilliseconds =
            static_cast<double>(timestamps[3] - timestamps[2]) * nanosecondsToMilliseconds;
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

    // 主机写参数缓冲。非一致内存上的写入要显式冲洗才能被设备看到
    writeParameterBuffer(ctx, frame, uniform);

    const VkPipelineStageFlags presentWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragmentWaitStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    uint64_t signalTimelineValue = 0;
    const bool singleCommandBuffer = input.syncMode == SYNC_MODE_BARRIER || input.syncMode == SYNC_MODE_EVENT ||
                                     input.syncMode == SYNC_MODE_SUBPASS;

    if (singleCommandBuffer) {
        VkCommandBuffer commandBuffer = frame.producerCommandBuffer;
        VK_CHECK(vkResetCommandBuffer(commandBuffer, 0));
        beginCommandBuffer(commandBuffer);
        resetFrameTimestamps(renderer, frame, commandBuffer, 0, 4);
        writeFrameTimestamp(renderer, frame, commandBuffer, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        const double producerStart = nowSeconds();
        if (input.syncMode == SYNC_MODE_BARRIER) {
            recordComputeProducer(commandBuffer, renderer, frame, ctx.swapchainExtent);
        } else if (input.syncMode == SYNC_MODE_EVENT) {
            // 事件复用，先复位再置位。复位与置位之间没有隐式顺序，靠这条命令缓冲里的
            // 提交顺序与组内资源围栏保证不与上一帧的等待重叠
            vkCmdResetEvent(commandBuffer, frame.patternEvent, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            recordComputeProducer(commandBuffer, renderer, frame, ctx.swapchainExtent);
        }
        outStatistics.cpuRecordProducerMilliseconds = (nowSeconds() - producerStart) * 1000.0;

        const double syncStart = nowSeconds();
        if (input.syncMode == SYNC_MODE_BARRIER) {
            recordPatternRelease(commandBuffer, renderer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
        } else if (input.syncMode == SYNC_MODE_EVENT) {
            // 设备上置位事件，再在片元阶段之前等待它。访问范围由等待命令给出：
            // 置位命令只带阶段掩码，不定义访问范围
            vkCmdSetEvent(commandBuffer, frame.patternEvent, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            VkImageMemoryBarrier barrier =
                makePatternBarrier(renderer, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT);
            vkCmdWaitEvents(commandBuffer, 1, &frame.patternEvent, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        outStatistics.cpuRecordSyncMilliseconds = (nowSeconds() - syncStart) * 1000.0;

        // 生产者段到此结束，接着是消费者段。子通道模式的两段都在渲染通道内部，
        // 时间戳由 recordSubpassConsumer 在两个子通道的交界处记
        if (input.syncMode != SYNC_MODE_SUBPASS) {
            writeFrameTimestamp(renderer, frame, commandBuffer, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            writeFrameTimestamp(renderer, frame, commandBuffer, 2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        }

        const double consumerStart = nowSeconds();
        if (input.syncMode == SYNC_MODE_SUBPASS) {
            recordSubpassConsumer(commandBuffer, ctx, renderer, frame, imageIndex, input, outStatistics);
        } else {
            recordSampledConsumer(commandBuffer, ctx, renderer, frame, imageIndex, input, outStatistics);
        }
        outStatistics.cpuRecordConsumerMilliseconds = (nowSeconds() - consumerStart) * 1000.0;

        const double captureStart = nowSeconds();
        if (input.captureBuffer != nullptr) {
            recordCaptureCopy(ctx, commandBuffer, imageIndex, *input.captureBuffer);
        }
        outStatistics.cpuRecordCaptureMilliseconds =
            input.captureBuffer != nullptr ? (nowSeconds() - captureStart) * 1000.0 : 0.0;

        const double submitStart = nowSeconds();
        writeFrameTimestamp(renderer, frame, commandBuffer, 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &presentWaitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderer.presentSemaphores[imageIndex];
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submitInfo, frame.inFlight));
        outStatistics.cpuRecordSubmitMilliseconds = (nowSeconds() - submitStart) * 1000.0;
        outStatistics.commandBufferCount = 1;
    } else {
        // 生产者提交
        VkCommandBuffer producerCommandBuffer = frame.producerCommandBuffer;
        VK_CHECK(vkResetCommandBuffer(producerCommandBuffer, 0));
        beginCommandBuffer(producerCommandBuffer);
        resetFrameTimestamps(renderer, frame, producerCommandBuffer, 0, 2);
        writeFrameTimestamp(renderer, frame, producerCommandBuffer, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        const double producerStart = nowSeconds();
        recordComputeProducer(producerCommandBuffer, renderer, frame, ctx.swapchainExtent);
        // 收尾屏障只负责把写入置为可用并转成只读布局，跨提交的那一半依赖交给下面选定的原语
        recordPatternRelease(producerCommandBuffer, renderer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
        outStatistics.cpuRecordProducerMilliseconds = (nowSeconds() - producerStart) * 1000.0;

        const double syncStart = nowSeconds();
        // 两次提交之间用哪种方式接上，由同步方式决定
        VkSubmitInfo producerSubmit = {};
        producerSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        producerSubmit.commandBufferCount = 1;
        producerSubmit.pCommandBuffers = &producerCommandBuffer;

        VkTimelineSemaphoreSubmitInfo producerTimeline = {};
        producerTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        producerTimeline.signalSemaphoreValueCount = 1;

        switch (input.syncMode) {
            case SYNC_MODE_SEMAPHORE_BINARY:
                producerSubmit.signalSemaphoreCount = 1;
                producerSubmit.pSignalSemaphores = &frame.patternReadyBinary;
                break;
            case SYNC_MODE_SEMAPHORE_TIMELINE:
                signalTimelineValue = frameCounter + 1;
                producerTimeline.pSignalSemaphoreValues = &signalTimelineValue;
                producerSubmit.pNext = &producerTimeline;
                producerSubmit.signalSemaphoreCount = 1;
                producerSubmit.pSignalSemaphores = &renderer.patternReadyTimeline;
                break;
            default:
                break;
        }

        writeFrameTimestamp(renderer, frame, producerCommandBuffer, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VK_CHECK(vkEndCommandBuffer(producerCommandBuffer));

        const VkFence producerFence = input.syncMode == SYNC_MODE_FENCE ? frame.patternSync : VK_NULL_HANDLE;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &producerSubmit, producerFence));
        outStatistics.cpuRecordSyncMilliseconds = (nowSeconds() - syncStart) * 1000.0;

        // 主机侧的等待
        const double hostWaitStart = nowSeconds();
        switch (input.syncMode) {
            case SYNC_MODE_SEMAPHORE_TIMELINE: {
                // 时间线信号量可以在主机上查询计数值，不阻塞在这里
                uint64_t counterValue = 0;
                VK_CHECK(renderer.getSemaphoreCounterValue(ctx.device, renderer.patternReadyTimeline,
                                                           &counterValue));
                outStatistics.timelineValue = counterValue;
                break;
            }
            case SYNC_MODE_FENCE:
                VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.patternSync, VK_TRUE, UINT64_MAX));
                VK_CHECK(vkResetFences(ctx.device, 1, &frame.patternSync));
                break;
            case SYNC_MODE_QUEUE_IDLE:
                VK_CHECK(vkQueueWaitIdle(ctx.queue));
                break;
            case SYNC_MODE_DEVICE_IDLE:
                VK_CHECK(vkDeviceWaitIdle(ctx.device));
                break;
            default:
                break;
        }
        outStatistics.hostWaitMilliseconds = (nowSeconds() - hostWaitStart) * 1000.0;

        // 消费者提交
        VkCommandBuffer consumerCommandBuffer = frame.consumerCommandBuffer;
        VK_CHECK(vkResetCommandBuffer(consumerCommandBuffer, 0));
        beginCommandBuffer(consumerCommandBuffer);
        resetFrameTimestamps(renderer, frame, consumerCommandBuffer, 2, 2);
        writeFrameTimestamp(renderer, frame, consumerCommandBuffer, 2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        const double consumerStart = nowSeconds();
        recordSampledConsumer(consumerCommandBuffer, ctx, renderer, frame, imageIndex, input, outStatistics);
        outStatistics.cpuRecordConsumerMilliseconds = (nowSeconds() - consumerStart) * 1000.0;

        const double captureStart = nowSeconds();
        if (input.captureBuffer != nullptr) {
            recordCaptureCopy(ctx, consumerCommandBuffer, imageIndex, *input.captureBuffer);
        }
        outStatistics.cpuRecordCaptureMilliseconds =
            input.captureBuffer != nullptr ? (nowSeconds() - captureStart) * 1000.0 : 0.0;

        const double submitStart = nowSeconds();
        writeFrameTimestamp(renderer, frame, consumerCommandBuffer, 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VK_CHECK(vkEndCommandBuffer(consumerCommandBuffer));

        VkSemaphore waitSemaphores[2] = { frame.imageAvailable, VK_NULL_HANDLE };
        VkPipelineStageFlags waitStages[2] = { presentWaitStage, fragmentWaitStage };
        VkTimelineSemaphoreSubmitInfo consumerTimeline = {};
        uint64_t waitTimelineValues[2] = { 0, 0 };
        consumerTimeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;

        uint32_t waitCount = 1;
        switch (input.syncMode) {
            case SYNC_MODE_SEMAPHORE_BINARY:
                waitSemaphores[1] = frame.patternReadyBinary;
                waitCount = 2;
                break;
            case SYNC_MODE_SEMAPHORE_TIMELINE:
                waitSemaphores[1] = renderer.patternReadyTimeline;
                waitTimelineValues[1] = frameCounter + 1;
                consumerTimeline.waitSemaphoreValueCount = 2;
                consumerTimeline.pWaitSemaphoreValues = waitTimelineValues;
                waitCount = 2;
                break;
            default:
                break;
        }

        VkSubmitInfo consumerSubmit = {};
        consumerSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        consumerSubmit.waitSemaphoreCount = waitCount;
        consumerSubmit.pWaitSemaphores = waitSemaphores;
        consumerSubmit.pWaitDstStageMask = waitStages;
        consumerSubmit.commandBufferCount = 1;
        consumerSubmit.pCommandBuffers = &consumerCommandBuffer;
        consumerSubmit.signalSemaphoreCount = 1;
        consumerSubmit.pSignalSemaphores = &renderer.presentSemaphores[imageIndex];
        if (input.syncMode == SYNC_MODE_SEMAPHORE_TIMELINE) {
            consumerSubmit.pNext = &consumerTimeline;
        }
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &consumerSubmit, frame.inFlight));
        outStatistics.cpuRecordSubmitMilliseconds = (nowSeconds() - submitStart) * 1000.0;
        outStatistics.commandBufferCount = 2;
        outStatistics.timelineValue = signalTimelineValue;
    }

    if (renderer.timestampsSupported) {
        frame.timestampsValid = true;
    }

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
