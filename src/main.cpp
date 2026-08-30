#include "frame_capture.h"
#include "obj_loader.h"
#include "renderer.h"
#include "scene.h"
#include "vk_check.h"
#include "vk_context.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    uint32_t instanceCount = 200000;
    uint32_t lightCount = 64;
    double autoExitSeconds = 0.0;
    std::string capturePath;
    DrawPath drawPath = DRAW_PATH_TRADITIONAL;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instances") == 0 && i + 1 < argc) {
            instanceCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--lights") == 0 && i + 1 < argc) {
            lightCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--indirect") == 0) {
            drawPath = DRAW_PATH_INDIRECT;
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

    std::vector<InstanceData> instances;
    buildInstances(instanceCount, 8.0f, instances);

    std::vector<LightData> lights(lightCount);

    Renderer renderer = {};
    createRenderer(ctx, renderer, mesh, instances, lightCount);

    Camera camera;
    initCamera(camera, instances);

    std::vector<uint32_t> visibleIndices(instances.size());

    const float aspectRatio =
        static_cast<float>(ctx.swapchainExtent.width) / static_cast<float>(ctx.swapchainExtent.height);

    uint64_t frameCounter = 0;
    double previousTime = glfwGetTime();
    double statisticsTime = previousTime;
    const double startTime = previousTime;
    uint32_t framesSinceStatistics = 0;

    // 统计量按窗口累加，显示时取平均
    double accumulatedCpuCull = 0.0;
    double accumulatedCpuRecord = 0.0;
    double accumulatedGpu = 0.0;
    uint32_t lastVisibleCount = 0;
    uint32_t lastDrawCallCount = 0;

    bool spaceWasPressed = false;

    GpuBuffer captureBuffer = {};
    bool captureRequested = !capturePath.empty();
    if (captureRequested) {
        createCaptureBuffer(ctx, captureBuffer);
    }
    bool captureDone = false;

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            break;
        }

        const bool spaceIsPressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceIsPressed && !spaceWasPressed) {
            drawPath = drawPath == DRAW_PATH_TRADITIONAL ? DRAW_PATH_INDIRECT : DRAW_PATH_TRADITIONAL;
            statisticsTime = glfwGetTime();
            framesSinceStatistics = 0;
            accumulatedCpuCull = 0.0;
            accumulatedCpuRecord = 0.0;
            accumulatedGpu = 0.0;
        }
        spaceWasPressed = spaceIsPressed;

        const double currentTime = glfwGetTime();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);
        previousTime = currentTime;

        updateCamera(camera, window, deltaSeconds);
        updateLights(camera.position, 48.0f, 110.0f, lights);

        CameraUniform cameraUniform;
        fillCameraUniform(camera, aspectRatio, static_cast<uint32_t>(instances.size()), mesh.boundsRadius,
                          static_cast<uint32_t>(lights.size()), cameraUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        FrameStatistics statistics = {};
        drawFrame(ctx, renderer, frameCounter, drawPath, cameraUniform, lights, instances, visibleIndices.data(),
                  mesh.boundsRadius, shouldCaptureThisFrame ? &captureBuffer : nullptr, statistics);
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;
        ++framesSinceStatistics;

        accumulatedCpuCull += statistics.cpuCullMilliseconds;
        accumulatedCpuRecord += statistics.cpuRecordMilliseconds;
        accumulatedGpu += statistics.gpuMilliseconds;
        lastVisibleCount = statistics.visibleInstanceCount;
        lastDrawCallCount = statistics.drawCallCount;

        if (currentTime - statisticsTime >= 0.5) {
            const double frameCount = static_cast<double>(framesSinceStatistics);
            const double averageFrameMilliseconds = (currentTime - statisticsTime) * 1000.0 / frameCount;
            char title[512];
            std::snprintf(title, sizeof(title),
                          "%s | 实例 %u | 可见 %u | 绘制命令 %u | 帧 %.2f ms (%.0f FPS) | 主机剔除 %.3f ms | "
                          "主机记录 %.3f ms | 设备 %.2f ms",
                          drawPath == DRAW_PATH_TRADITIONAL ? "传统 drawIndexed" : "indirect + 计算着色器剔除",
                          static_cast<uint32_t>(instances.size()), lastVisibleCount, lastDrawCallCount,
                          averageFrameMilliseconds, 1000.0 / averageFrameMilliseconds,
                          accumulatedCpuCull / frameCount, accumulatedCpuRecord / frameCount,
                          accumulatedGpu / frameCount);
            glfwSetWindowTitle(window, title);
            std::printf("%s\n", title);

            statisticsTime = currentTime;
            framesSinceStatistics = 0;
            accumulatedCpuCull = 0.0;
            accumulatedCpuRecord = 0.0;
            accumulatedGpu = 0.0;
        }

        if (shouldExit) {
            break;
        }
    }

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    if (captureRequested) {
        writeCaptureBufferToPng(ctx, captureBuffer, capturePath);
        destroyBuffer(ctx, captureBuffer);
    }

    std::printf("共提交 %llu 帧, 最后一帧可见实例 %u, 绘制命令 %u\n",
                static_cast<unsigned long long>(frameCounter), lastVisibleCount, lastDrawCallCount);

    destroyRenderer(ctx, renderer);
    destroySwapchain(ctx);
    destroyVulkanContext(ctx);

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
