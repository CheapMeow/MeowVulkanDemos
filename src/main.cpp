#include "obj_loader.h"
#include "vk_check.h"
#include "vk_context.h"

#include <cstring>
#include <vector>

enum { FRAMES_IN_FLIGHT = 2 };

struct FrameSync {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkFence inFlight;
};

static VkRenderPass createPresentRenderPass(const VulkanContext& ctx)
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = ctx.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(ctx.device, &renderPassInfo, nullptr, &renderPass));
    return renderPass;
}

int main(int argc, char** argv)
{
    double autoExitSeconds = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        }
    }

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit 失败");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("当前环境没有可用的 Vulkan 加载器");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Indirect Draw Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("创建窗口失败");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, window, true);
    createSwapchain(ctx);

    MeshData mesh;
    loadObj(std::string(PROJECT_ROOT_DIR) + "/assets/backpack/backpack.obj", mesh);

    VkRenderPass renderPass = createPresentRenderPass(ctx);

    std::vector<VkFramebuffer> framebuffers(ctx.swapchainImageCount);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &ctx.swapchainImageViews[i];
        framebufferInfo.width = ctx.swapchainExtent.width;
        framebufferInfo.height = ctx.swapchainExtent.height;
        framebufferInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(ctx.device, &framebufferInfo, nullptr, &framebuffers[i]));
    }

    FrameSync frames[FRAMES_IN_FLIGHT] = {};
    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = ctx.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &allocInfo, &frames[i].commandBuffer));

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &frames[i].imageAvailable));

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(ctx.device, &fenceInfo, nullptr, &frames[i].inFlight));
    }

    // 呈现用信号量按交换链图像索引区分，避免图像未重新取得时重复使用同一信号量
    std::vector<VkSemaphore> presentSemaphores(ctx.swapchainImageCount);
    std::vector<VkFence> imageFences(ctx.swapchainImageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(ctx.device, &semaphoreInfo, nullptr, &presentSemaphores[i]));
    }

    const double startTime = glfwGetTime();
    uint64_t frameIndex = 0;

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            break;
        }

        FrameSync& frame = frames[frameIndex % FRAMES_IN_FLIGHT];
        VK_CHECK(vkWaitForFences(ctx.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

        uint32_t imageIndex = 0;
        VK_CHECK(vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX, frame.imageAvailable,
                                       VK_NULL_HANDLE, &imageIndex));

        if (imageFences[imageIndex] != VK_NULL_HANDLE) {
            VK_CHECK(vkWaitForFences(ctx.device, 1, &imageFences[imageIndex], VK_TRUE, UINT64_MAX));
        }
        imageFences[imageIndex] = frame.inFlight;

        VK_CHECK(vkResetFences(ctx.device, 1, &frame.inFlight));
        VK_CHECK(vkResetCommandBuffer(frame.commandBuffer, 0));

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 0.05f;
        clearValue.color.float32[1] = 0.06f;
        clearValue.color.float32[2] = 0.09f;
        clearValue.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo renderPassBegin = {};
        renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBegin.renderPass = renderPass;
        renderPassBegin.framebuffer = framebuffers[imageIndex];
        renderPassBegin.renderArea.extent = ctx.swapchainExtent;
        renderPassBegin.clearValueCount = 1;
        renderPassBegin.pClearValues = &clearValue;
        vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(frame.commandBuffer);
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
        submitInfo.pSignalSemaphores = &presentSemaphores[imageIndex];
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submitInfo, frame.inFlight));

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &presentSemaphores[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &ctx.swapchain;
        presentInfo.pImageIndices = &imageIndex;
        VK_CHECK(vkQueuePresentKHR(ctx.queue, &presentInfo));

        ++frameIndex;
        if (autoExitSeconds > 0.0 && glfwGetTime() - startTime >= autoExitSeconds) {
            break;
        }
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    std::printf("共提交 %llu 帧\n", static_cast<unsigned long long>(frameIndex));

    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        vkDestroyFence(ctx.device, frames[i].inFlight, nullptr);
        vkDestroySemaphore(ctx.device, frames[i].imageAvailable, nullptr);
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &frames[i].commandBuffer);
    }
    for (uint32_t i = 0; i < ctx.swapchainImageCount; ++i) {
        vkDestroySemaphore(ctx.device, presentSemaphores[i], nullptr);
    }
    for (uint32_t i = 0; i < framebuffers.size(); ++i) {
        vkDestroyFramebuffer(ctx.device, framebuffers[i], nullptr);
    }
    vkDestroyRenderPass(ctx.device, renderPass, nullptr);
    destroySwapchain(ctx);
    destroyVulkanContext(ctx);

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
