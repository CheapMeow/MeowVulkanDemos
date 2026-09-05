#include "console.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
#include "renderer.h"
#include "scene.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 统计行与测量报告里的路径名称
static const char* drawPathName(DrawPath drawPath)
{
    if (drawPath == DRAW_PATH_TRADITIONAL) {
        return "per-instance drawIndexed";
    }
    if (drawPath == DRAW_PATH_INSTANCED) {
        return "instanced drawIndexed";
    }
    return "indirect + compute shader culling";
}

int main(int argc, char** argv)
{
    configureConsoleEncoding();

    uint32_t initialInstanceCount = 200000;
    uint32_t initialLightCount = 64;
    uint32_t lightCapacity = 256;
    double autoExitSeconds = 0.0;
    std::string capturePath;
    std::string reportPath;
    DrawPath drawPath = DRAW_PATH_TRADITIONAL;
    float farPlane = 160.0f;
    double switchEverySeconds = 0.0;
    bool interfaceEnabled = true;
    double sweepEverySeconds = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instances") == 0 && i + 1 < argc) {
            initialInstanceCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--lights") == 0 && i + 1 < argc) {
            initialLightCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--instanced") == 0) {
            drawPath = DRAW_PATH_INSTANCED;
        } else if (std::strcmp(argv[i], "--indirect") == 0) {
            drawPath = DRAW_PATH_INDIRECT;
        } else if (std::strcmp(argv[i], "--far") == 0 && i + 1 < argc) {
            farPlane = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--switch-every") == 0 && i + 1 < argc) {
            switchEverySeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--no-interface") == 0) {
            interfaceEnabled = false;
        } else if (std::strcmp(argv[i], "--sweep-instances") == 0 && i + 1 < argc) {
            sweepEverySeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
        }
    }

    // 与 Vulkan、窗口无关，尽早探测，探测不到就在面板里如实显示，不阻止程序继续运行
    GpuClockLockState gpuClockLockState;
    detectGpuClockLockState(gpuClockLockState);

    // 缓冲按容量分配，界面上滑动实例数量时不需要重建任何资源
    const uint32_t instanceCapacity = std::max(1000000u, initialInstanceCount);
    lightCapacity = std::max(lightCapacity, initialLightCount);

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit failed");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("no Vulkan loader is available in this environment");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Indirect Draw Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, window, true);
    createSwapchain(ctx);

    MeshData mesh;
    loadObj(std::string(PROJECT_ROOT_DIR) + "/assets/backpack/backpack.obj", mesh);

    std::vector<InstanceData> instances;
    buildInstances(instanceCapacity, 8.0f, instances);

    std::vector<LightData> lights(lightCapacity);

    Renderer renderer = {};
    createRenderer(ctx, renderer, mesh, instances, lightCapacity);

    UserInterface ui = {};
    createUserInterface(ctx, renderer, ui);

    Camera camera;
    initCamera(camera, instances);

    UiState uiState = {};
    uiState.drawPath = drawPath;
    uiState.activeInstanceCount = static_cast<int>(initialInstanceCount);
    uiState.activeLightCount = static_cast<int>(initialLightCount);
    uiState.farPlane = farPlane;
    uiState.cameraMoveSpeed = camera.moveSpeed;

    UiStatistics uiStatistics = {};

    std::vector<uint32_t> visibleIndices(instanceCapacity);

    const float aspectRatio =
        static_cast<float>(ctx.swapchainExtent.width) / static_cast<float>(ctx.swapchainExtent.height);

    uint64_t frameCounter = 0;
    double previousTime = glfwGetTime();
    double statisticsTime = previousTime;
    const double startTime = previousTime;
    uint32_t framesSinceStatistics = 0;

    // 统计量按窗口累加，每隔一段时间取平均后送进界面
    double accumulatedCpuCull = 0.0;
    double accumulatedCpuRecord = 0.0;
    double accumulatedGpu = 0.0;

    bool spaceWasPressed = false;
    double lastSwitchTime = previousTime;

    // 测量报告的累加窗口，跳过起始的预热阶段
    const double warmUpSeconds = 1.5;
    double reportWindowStart = 0.0;
    uint32_t reportFrameCount = 0;
    double reportCpuCull = 0.0;
    double reportCpuRecord = 0.0;
    double reportGpu = 0.0;
    uint32_t reportVisibleCount = 0;
    uint32_t reportDrawCallCount = 0;

    GpuBuffer captureBuffer = {};
    const bool captureRequested = !capturePath.empty();
    if (captureRequested) {
        createCaptureBuffer(ctx, captureBuffer);
    }
    bool captureDone = false;

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        if (interfaceEnabled) {
            beginUserInterfaceFrame();
            pollLiveGpuClocks(gpuClockLockState, glfwGetTime());
            buildUserInterface(uiState, uiStatistics, static_cast<int>(instanceCapacity),
                               static_cast<int>(lightCapacity), gpuClockLockState);
        }

        const bool keyboardGoesToInterface = interfaceEnabled && userInterfaceWantsKeyboard();
        if (!keyboardGoesToInterface && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (interfaceEnabled) {
                endUserInterfaceFrame();
            }
            break;
        }

        const bool spaceIsPressed =
            !keyboardGoesToInterface && glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        const bool switchByTimer =
            switchEverySeconds > 0.0 && glfwGetTime() - lastSwitchTime >= switchEverySeconds;
        if ((spaceIsPressed && !spaceWasPressed) || switchByTimer) {
            // 三条路径依次轮换
            uiState.drawPath = static_cast<DrawPath>((static_cast<int>(uiState.drawPath) + 1) % DRAW_PATH_COUNT);
            lastSwitchTime = glfwGetTime();
            statisticsTime = lastSwitchTime;
            framesSinceStatistics = 0;
            accumulatedCpuCull = 0.0;
            accumulatedCpuRecord = 0.0;
            accumulatedGpu = 0.0;
        }
        spaceWasPressed = spaceIsPressed;

        const double currentTime = glfwGetTime();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);
        previousTime = currentTime;

        // 自动在若干实例数量之间轮换，用来验证运行中改变数量的正确性
        if (sweepEverySeconds > 0.0) {
            const int sweepValues[4] = { 1000, 50000, 300000, static_cast<int>(instanceCapacity) };
            const int sweepIndex = static_cast<int>((currentTime - startTime) / sweepEverySeconds) % 4;
            uiState.activeInstanceCount = sweepValues[sweepIndex];
        }

        camera.farPlane = uiState.farPlane;
        camera.moveSpeed = uiState.cameraMoveSpeed;
        if (!keyboardGoesToInterface) {
            updateCamera(camera, window, deltaSeconds);
        }

        const uint32_t activeInstanceCount = static_cast<uint32_t>(uiState.activeInstanceCount);
        const uint32_t activeLightCount = static_cast<uint32_t>(uiState.activeLightCount);
        updateLights(camera.position, activeLightCount, 48.0f, 110.0f, lights);

        CameraUniform cameraUniform;
        fillCameraUniform(camera, aspectRatio, activeInstanceCount, mesh.boundsRadius, activeLightCount,
                          cameraUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawPath = uiState.drawPath;
        input.activeInstanceCount = activeInstanceCount;
        input.activeLightCount = activeLightCount;
        input.drawUserInterface = interfaceEnabled;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        drawFrame(ctx, renderer, frameCounter, input, cameraUniform, lights, instances, visibleIndices.data(),
                  statistics);
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;
        ++framesSinceStatistics;

        accumulatedCpuCull += statistics.cpuCullMilliseconds;
        accumulatedCpuRecord += statistics.cpuRecordMilliseconds;
        accumulatedGpu += statistics.gpuMilliseconds;
        uiStatistics.visibleInstanceCount = statistics.visibleInstanceCount;
        uiStatistics.drawCallCount = statistics.drawCallCount;

        if (currentTime - startTime >= warmUpSeconds) {
            if (reportFrameCount == 0) {
                reportWindowStart = currentTime;
            }
            ++reportFrameCount;
            reportCpuCull += statistics.cpuCullMilliseconds;
            reportCpuRecord += statistics.cpuRecordMilliseconds;
            reportGpu += statistics.gpuMilliseconds;
            reportVisibleCount = statistics.visibleInstanceCount;
            reportDrawCallCount = statistics.drawCallCount;
        }

        if (currentTime - statisticsTime >= 0.25) {
            const double frameCount = static_cast<double>(framesSinceStatistics);
            uiStatistics.frameMilliseconds = (currentTime - statisticsTime) * 1000.0 / frameCount;
            uiStatistics.cpuCullMilliseconds = accumulatedCpuCull / frameCount;
            uiStatistics.cpuRecordMilliseconds = accumulatedCpuRecord / frameCount;
            uiStatistics.gpuMilliseconds = accumulatedGpu / frameCount;

            std::printf("%s | instances %u | visible %u | draw commands %u | frame %.2f ms (%.0f FPS) | CPU cull %.3f ms | "
                        "CPU record %.3f ms | GPU %.2f ms\n",
                        drawPathName(uiState.drawPath),
                        activeInstanceCount, uiStatistics.visibleInstanceCount, uiStatistics.drawCallCount,
                        uiStatistics.frameMilliseconds, 1000.0 / uiStatistics.frameMilliseconds,
                        uiStatistics.cpuCullMilliseconds, uiStatistics.cpuRecordMilliseconds,
                        uiStatistics.gpuMilliseconds);

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

    std::printf("submitted %llu frames, last frame visible %u, draw commands %u\n",
                static_cast<unsigned long long>(frameCounter), uiStatistics.visibleInstanceCount,
                uiStatistics.drawCallCount);

    // 测量报告由程序自己写入，不依赖控制台重定向
    if (!reportPath.empty()) {
        if (reportFrameCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }

        const double windowSeconds = glfwGetTime() - reportWindowStart;
        const double frameCount = static_cast<double>(reportFrameCount);

        // 追加写入前判断文件是否为空，为空则先写一行表头
        std::FILE* probeFile = std::fopen(reportPath.c_str(), "rb");
        bool needsHeader = true;
        if (probeFile != nullptr) {
            std::fseek(probeFile, 0, SEEK_END);
            needsHeader = std::ftell(probeFile) == 0;
            std::fclose(probeFile);
        }

        std::FILE* reportFile = std::fopen(reportPath.c_str(), "a");
        if (reportFile == nullptr) {
            FATAL("failed to open measurement report file: %s", reportPath.c_str());
        }
        if (needsHeader) {
            std::fprintf(reportFile,
                         "draw_path,instances,visible_instances,draw_commands,frame_ms,cpu_cull_ms,"
                         "cpu_record_ms,gpu_ms\n");
        }
        std::fprintf(reportFile, "%s,%u,%u,%u,%.3f,%.3f,%.3f,%.3f\n", drawPathName(uiState.drawPath),
                     static_cast<uint32_t>(uiState.activeInstanceCount), reportVisibleCount,
                     reportDrawCallCount, windowSeconds * 1000.0 / frameCount, reportCpuCull / frameCount,
                     reportCpuRecord / frameCount, reportGpu / frameCount);
        std::fclose(reportFile);
    }

    destroyUserInterface(ctx, ui);
    destroyRenderer(ctx, renderer);
    destroySwapchain(ctx);
    destroyVulkanContext(ctx);

    glfwDestroyWindow(window);
    glfwTerminate();

    releaseGpuClockLockOnExit(gpuClockLockState);

    restoreConsoleEncoding();
    return EXIT_SUCCESS;
}
