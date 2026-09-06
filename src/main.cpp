#include "asset_file.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
#include "rdoc_trigger.h"
#include "renderer.h"
#include "scene.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建。
// 抓帧缓冲的大小也由交换链尺寸决定，抓帧还没发生时一并重建
static void rebuildSwapchainResources(VulkanContext& ctx, Renderer& renderer, GpuBuffer& captureBuffer,
                                      bool capturePending)
{
    recreateSwapchain(ctx);
    recreateSwapchainTargets(ctx, renderer);

    if (capturePending) {
        if (captureBuffer.buffer != VK_NULL_HANDLE) {
            destroyBuffer(ctx, captureBuffer);
        }
        createCaptureBuffer(ctx, captureBuffer);
    }
}

// 把一行数据追加到报告文件，文件为空时先写一行表头
static void appendReportLine(const std::string& reportPath, const std::string& line)
{
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
        std::fprintf(reportFile, "draw_path,instances,visible_instances,draw_commands");
        for (int i = 0; i < TIMING_ID_COUNT; ++i) {
            const char* columnName = timingReportColumnName(static_cast<TimingId>(i));
            std::fprintf(reportFile, ",%s_avg,%s_stddev", columnName, columnName);
        }
        std::fprintf(reportFile, "\n");
    }
    std::fprintf(reportFile, "%s\n", line.c_str());
    std::fclose(reportFile);
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
    uint16_t controlPort = 0;
    uint32_t requestedCoreClockMHz = 0;
    uint32_t requestedMemoryClockMHz = 0;

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
        } else if (std::strcmp(argv[i], "--control-port") == 0 && i + 1 < argc) {
            controlPort = static_cast<uint16_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--core-clock") == 0 && i + 1 < argc) {
            requestedCoreClockMHz = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--memory-clock") == 0 && i + 1 < argc) {
            requestedMemoryClockMHz = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        }
    }

    // 与 Vulkan、窗口无关，尽早探测，探测不到就在面板里如实显示，不阻止程序继续运行
    GpuClockLockState gpuClockLockState;
    detectGpuClockLockState(gpuClockLockState);

    // 命令行给了锁频目标就在启动阶段锁上：档位列表里取最接近请求值的一项，
    // 实际锁到的值与请求值不同时会如实打印
    const bool coreClockRequested = requestedCoreClockMHz > 0;
    const bool memoryClockRequested = requestedMemoryClockMHz > 0;
    if (coreClockRequested != memoryClockRequested) {
        FATAL("--core-clock and --memory-clock must be given together");
    }
    if (coreClockRequested) {
        requestGpuClockLockFromCommandLine(gpuClockLockState, requestedCoreClockMHz, requestedMemoryClockMHz);
    }

    // 频率曲线由后台线程采样，测量模式下不启动，避免采样本身干扰被测的帧时间
    GpuClockMonitor gpuClockMonitor;

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
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    MeshData mesh;
    loadObjFromMemory(readAssetBytes("backpack/backpack.obj"), mesh);

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

    uint64_t frameCounter = 0;
    double previousTime = nowSeconds();
    double lastPrintTime = previousTime;
    const double startTime = previousTime;

    TimingStore timingStore;
    initTimingStore(timingStore, startTime);

    bool spaceWasPressed = false;
    double lastSwitchTime = previousTime;

    // 测量报告跳过起始的预热阶段，只统计预热结束之后到程序退出的样本
    const double warmUpSeconds = 1.5;
    uint32_t reportVisibleCount = 0;
    uint32_t reportDrawCallCount = 0;

    GpuBuffer captureBuffer = {};
    const bool captureRequested = !capturePath.empty();
    if (captureRequested) {
        createCaptureBuffer(ctx, captureBuffer);
    }
    bool captureDone = false;

    // TCP 控制服务：外部测量脚本连接后逐条发命令，配置、分段测量、截帧都在这里执行。
    // 命令在主线程的 pump 里处理，与渲染线程天然串行，不需要额外加锁
    ControlServer controlServer;
    bool usedSegments = false;      // 用过 begin/end 分段测量后，退出不再写整段报告
    bool segmentActive = false;     // 当前是否处于 begin 到 end 的测量分段
    double segmentStartSeconds = 0.0;
    bool quitRequested = false;

    controlServer.onCommand = [&](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "path") {
            std::string value;
            stream >> value;
            DrawPath newPath = DRAW_PATH_TRADITIONAL;
            if (value == "traditional" || value == "0") {
                newPath = DRAW_PATH_TRADITIONAL;
            } else if (value == "instanced" || value == "1") {
                newPath = DRAW_PATH_INSTANCED;
            } else if (value == "indirect" || value == "2") {
                newPath = DRAW_PATH_INDIRECT;
            } else {
                return "err: unknown path";
            }
            uiState.drawPath = newPath;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "instances") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(instanceCapacity)) {
                return "err: instances out of range";
            }
            uiState.activeInstanceCount = static_cast<int>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "lights") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(lightCapacity)) {
                return "err: lights out of range";
            }
            uiState.activeLightCount = static_cast<int>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "far") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: bad far plane";
            }
            uiState.farPlane = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "begin") {
            usedSegments = true;
            segmentActive = true;
            segmentStartSeconds = nowSeconds();
            reportVisibleCount = 0;
            reportDrawCallCount = 0;
            resetTimingReport(timingStore);
            return "ok";
        }
        if (verb == "end") {
            if (!segmentActive) {
                return "err: no segment started";
            }
            if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
                return "err: segment had no sampled frames";
            }
            const std::string line = timingReportLine(
                drawPathName(uiState.drawPath), static_cast<uint32_t>(uiState.activeInstanceCount),
                reportVisibleCount, reportDrawCallCount, timingStore);
            if (!reportPath.empty()) {
                appendReportLine(reportPath, line);
            }
            segmentActive = false;
            resetTimingReport(timingStore);
            return "row " + line;
        }
        if (verb == "capture") {
            renderdocTriggerCapture();
            return "ok";
        }
        if (verb == "quit") {
            quitRequested = true;
            return "ok";
        }
        return "err: unknown command";
    };

    if (controlPort != 0 && !controlServer.start(controlPort)) {
        FATAL("failed to open the TCP control server on port %u", controlPort);
    }

    if (interfaceEnabled) {
        startGpuClockMonitor(gpuClockMonitor, gpuClockLockState, startTime);
    }

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        // 处理 TCP 控制命令，quit 请求立即退出
        controlServer.pump();
        if (quitRequested) {
            break;
        }

        // 窗口最小化时表面尺寸为零，没有可以绘制的内容，等窗口恢复
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth == 0 || framebufferHeight == 0) {
            if (autoExitSeconds > 0.0 && nowSeconds() - startTime >= autoExitSeconds) {
                break;
            }
            continue;
        }

        if (static_cast<uint32_t>(framebufferWidth) != ctx.swapchainExtent.width ||
            static_cast<uint32_t>(framebufferHeight) != ctx.swapchainExtent.height) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
        }

        if (interfaceEnabled) {
            beginUserInterfaceFrame();
            buildUserInterface(uiState, uiStatistics, timingStore, static_cast<int>(instanceCapacity),
                               static_cast<int>(lightCapacity), gpuClockLockState, gpuClockMonitor);
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
            switchEverySeconds > 0.0 && nowSeconds() - lastSwitchTime >= switchEverySeconds;
        if ((spaceIsPressed && !spaceWasPressed) || switchByTimer) {
            // 三条路径依次轮换
            uiState.drawPath = static_cast<DrawPath>((static_cast<int>(uiState.drawPath) + 1) % DRAW_PATH_COUNT);
            lastSwitchTime = nowSeconds();
            resetTimingWindows(timingStore);
        }
        spaceWasPressed = spaceIsPressed;

        const double currentTime = nowSeconds();
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
            CameraInput cameraInput = {};
            cameraInput.turnLeft = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            cameraInput.turnRight = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            cameraInput.turnUp = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
            cameraInput.turnDown = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
            cameraInput.moveForward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
            cameraInput.moveBack = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
            cameraInput.moveLeft = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
            cameraInput.moveRight = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
            cameraInput.moveUp = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
            cameraInput.moveDown = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
            cameraInput.fast = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            updateCamera(camera, cameraInput, deltaSeconds);
        }

        const uint32_t activeInstanceCount = static_cast<uint32_t>(uiState.activeInstanceCount);
        const uint32_t activeLightCount = static_cast<uint32_t>(uiState.activeLightCount);
        updateLights(camera.position, activeLightCount, 48.0f, 110.0f, lights);

        // 宽高比跟着交换链走，窗口尺寸变化后立刻生效
        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

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
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, cameraUniform, lights, instances,
                                          visibleIndices.data(), statistics);
        if (!frameDrawn) {
            // 交换链在获取图像或者呈现时失效，重建后从下一帧继续，本帧不计入统计
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.visibleInstanceCount = statistics.visibleInstanceCount;
        uiStatistics.drawCallCount = statistics.drawCallCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);
        if (includeInReport) {
            reportVisibleCount = statistics.visibleInstanceCount;
            reportDrawCallCount = statistics.drawCallCount;
        }

        // 按 TimingId 的顺序填满全部计时项，新增计时项时只需要在这里补一行
        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_CULL] = statistics.cpuCullMilliseconds;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_CULL_DISPATCH] = statistics.cpuRecordCullDispatchMilliseconds;
        timingValues[TIMING_CPU_RECORD_GBUFFER_PASS] = statistics.cpuRecordGBufferPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_LIGHTING_PASS] = statistics.cpuRecordLightingPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.25) {
            std::printf("%s | instances %u | visible %u | draw commands %u\n", drawPathName(uiState.drawPath),
                        activeInstanceCount, uiStatistics.visibleInstanceCount, uiStatistics.drawCallCount);
            for (int i = 0; i < TIMING_ID_COUNT; ++i) {
                const TimingId id = static_cast<TimingId>(i);
                const TimingWindow& window = timingStore.window[id];
                if (id == TIMING_FRAME) {
                    const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
                    std::printf("  %-30s %7.3f +/- %6.3f ms (%.0f FPS)\n", timingReportColumnName(id), window.mean,
                                window.standardDeviation, fps);
                } else {
                    std::printf("  %-30s %7.3f +/- %6.3f ms\n", timingReportColumnName(id), window.mean,
                                window.standardDeviation);
                }
            }
            lastPrintTime = currentTime;
        }

        if (shouldExit) {
            break;
        }
    }

    controlServer.stop();
    stopGpuClockMonitor(gpuClockMonitor);

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    if (captureRequested) {
        writeCaptureBufferToPng(ctx, captureBuffer, capturePath);
        destroyBuffer(ctx, captureBuffer);
    }

    std::printf("submitted %llu frames, last frame visible %u, draw commands %u\n",
                static_cast<unsigned long long>(frameCounter), uiStatistics.visibleInstanceCount,
                uiStatistics.drawCallCount);

    // 测量报告由程序自己写入，不依赖控制台重定向。分段测量时每一段在 end 命令时写一行，
    // 这里只处理没有使用分段测量的整段运行
    if (!reportPath.empty() && !usedSegments) {
        if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }
        const std::string line = timingReportLine(drawPathName(uiState.drawPath),
                                                 static_cast<uint32_t>(uiState.activeInstanceCount),
                                                 reportVisibleCount, reportDrawCallCount, timingStore);
        appendReportLine(reportPath, line);
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
