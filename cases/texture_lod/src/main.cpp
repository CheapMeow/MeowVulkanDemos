#include "asset_file.h"
#include "case_ui.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "renderer.h"
#include "scene.h"
#include "scene_setup.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 地面的半边长与相机初始位置
static const float GROUND_HALF_EXTENT = 60.0f;

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS = "view,mipmapped,anisotropy,lod_bias,draw_commands";

static const char* viewName(uint32_t mode)
{
    if (mode == LOD_VIEW_LOD) {
        return "lod";
    }
    if (mode == LOD_VIEW_FOOTPRINT) {
        return "footprint";
    }
    return "normal";
}

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[128];
    std::snprintf(prefix, sizeof(prefix), "%s,%d,%.0f,%.2f,%u", viewName(state.viewMode),
                  state.mipmapped ? 1 : 0, static_cast<double>(state.maxAnisotropy),
                  static_cast<double>(state.lodBias), drawCommands);
    return std::string(prefix);
}

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建
static void rebuildSwapchainResources(VulkanContext& ctx, LodRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillSceneUniform(const Camera& camera, float aspectRatio, uint32_t viewMode, uint32_t viewportWidth,
                             LodSceneUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.options = glm::vec4(static_cast<float>(viewMode), 0.0f, static_cast<float>(viewportWidth), 0.0f);
}

static void applyCameraPitch(Camera& camera, float pitchDegrees)
{
    camera.pitch = -glm::radians(pitchDegrees);
}

int main(int argc, char** argv)
{
    configureConsoleEncoding();

    double autoExitSeconds = 0.0;
    std::string capturePath;
    std::string reportPath;
    bool interfaceEnabled = true;
    uint16_t controlPort = 0;
    uint32_t requestedCoreClockMHz = 0;
    uint32_t requestedMemoryClockMHz = 0;
    uint32_t viewMode = LOD_VIEW_NORMAL;
    bool mipmapped = true;
    float lodBias = 0.0f;
    float maxAnisotropy = 1.0f;
    float pitchDegrees = 26.0f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--no-interface") == 0) {
            interfaceEnabled = false;
        } else if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            const char* value = argv[i + 1];
            if (std::strcmp(value, "normal") == 0) {
                viewMode = LOD_VIEW_NORMAL;
            } else if (std::strcmp(value, "lod") == 0) {
                viewMode = LOD_VIEW_LOD;
            } else if (std::strcmp(value, "footprint") == 0) {
                viewMode = LOD_VIEW_FOOTPRINT;
            } else {
                FATAL("--view takes normal, lod or footprint");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--no-mipmap") == 0) {
            mipmapped = false;
        } else if (std::strcmp(argv[i], "--anisotropy") == 0 && i + 1 < argc) {
            maxAnisotropy = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--lod-bias") == 0 && i + 1 < argc) {
            lodBias = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
            pitchDegrees = static_cast<float>(std::atof(argv[i + 1]));
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

    if (pitchDegrees < 8.0f || pitchDegrees > 75.0f) {
        FATAL("--pitch must be between 8 and 75 degrees");
    }
    if (lodBias < -2.0f || lodBias > 2.0f) {
        FATAL("--lod-bias must be between -2 and 2");
    }
    if (maxAnisotropy < 1.0f || maxAnisotropy > 16.0f) {
        FATAL("--anisotropy must be between 1 and 16");
    }

    // 与 Vulkan、窗口无关，尽早探测，探测不到就在面板里如实显示，不阻止程序继续运行
    GpuClockLockState gpuClockLockState;
    detectGpuClockLockState(gpuClockLockState);

    const bool coreClockRequested = requestedCoreClockMHz > 0;
    const bool memoryClockRequested = requestedMemoryClockMHz > 0;
    if (coreClockRequested != memoryClockRequested) {
        FATAL("--core-clock and --memory-clock must be given together");
    }
    if (coreClockRequested) {
        requestGpuClockLockFromCommandLine(gpuClockLockState, requestedCoreClockMHz, requestedMemoryClockMHz);
    }

    GpuClockMonitor gpuClockMonitor;

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit failed");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("no Vulkan loader is available in this environment");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Texture LOD Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    MeshData groundMesh;
    buildGroundQuadMesh(GROUND_HALF_EXTENT, groundMesh);

    LodRenderer renderer = {};
    createRenderer(ctx, renderer, groundMesh);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.renderPass, caseTexts, caseTextCount, ui);

    Camera camera = {};
    camera.position = glm::vec3(0.0f, 40.0f, 90.0f);
    camera.yaw = 0.0f;
    applyCameraPitch(camera, pitchDegrees);
    camera.verticalFieldOfView = glm::radians(60.0f);
    camera.nearPlane = 0.5f;
    camera.farPlane = 1000.0f;
    camera.moveSpeed = 20.0f;

    UiState uiState = {};
    uiState.cameraMoveSpeed = camera.moveSpeed;
    uiState.cameraPitchDegrees = pitchDegrees;
    uiState.mipmapped = mipmapped;
    uiState.lodBias = lodBias;
    uiState.maxAnisotropy = maxAnisotropy;
    uiState.viewMode = viewMode;

    UiStatistics uiStatistics = {};

    uint64_t frameCounter = 0;
    double previousTime = nowSeconds();
    double lastPrintTime = previousTime;
    const double startTime = previousTime;

    TimingStore timingStore;
    initTimingStore(timingStore, caseTimingItems(), TIMING_ID_COUNT, startTime);
    const std::string reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(timingStore);

    // 测量报告跳过起始的预热阶段，只统计预热结束之后到程序退出的样本
    const double warmUpSeconds = 1.5;
    uint32_t reportDrawCallCount = 0;

    GpuBuffer captureBuffer = {};
    const bool captureRequested = !capturePath.empty();
    if (captureRequested) {
        createCaptureBuffer(ctx, captureBuffer);
    }
    bool captureDone = false;

    ControlServer controlServer;
    bool usedSegments = false;
    bool segmentActive = false;
    double segmentStartSeconds = 0.0;
    bool quitRequested = false;

    controlServer.onCommand = [&](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "view") {
            std::string value;
            if (!(stream >> value) || (value != "normal" && value != "lod" && value != "footprint")) {
                return "err: view takes normal, lod or footprint";
            }
            uiState.viewMode = value == "lod" ? LOD_VIEW_LOD
                              : value == "footprint" ? LOD_VIEW_FOOTPRINT
                                                     : LOD_VIEW_NORMAL;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "mipmap") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: mipmap takes 0 or 1";
            }
            uiState.mipmapped = value == 1;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "anisotropy") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 16.0) {
                return "err: anisotropy out of range";
            }
            uiState.maxAnisotropy = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "lod-bias") {
            double value = 0.0;
            if (!(stream >> value) || value < -2.0 || value > 2.0) {
                return "err: lod-bias out of range";
            }
            uiState.lodBias = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "pitch") {
            double value = 0.0;
            if (!(stream >> value) || value < 8.0 || value > 75.0) {
                return "err: pitch out of range";
            }
            uiState.cameraPitchDegrees = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "begin") {
            usedSegments = true;
            segmentActive = true;
            segmentStartSeconds = nowSeconds();
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
            const std::string line =
                reportPrefix(uiState, reportDrawCallCount) + timingReportValueColumns(timingStore);
            if (!reportPath.empty()) {
                appendMeasurementReport(reportPath, reportHeaderColumns, line);
            }
            segmentActive = false;
            resetTimingReport(timingStore);
            return "row " + line;
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

        controlServer.pump();
        if (quitRequested) {
            break;
        }

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
            buildUserInterface(uiState, uiStatistics, timingStore, gpuClockLockState, gpuClockMonitor);
        }

        const bool keyboardGoesToInterface = interfaceEnabled && userInterfaceWantsKeyboard();
        if (!keyboardGoesToInterface && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (interfaceEnabled) {
                endUserInterfaceFrame();
            }
            break;
        }

        const double currentTime = nowSeconds();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);
        previousTime = currentTime;

        camera.moveSpeed = uiState.cameraMoveSpeed;
        applyCameraPitch(camera, uiState.cameraPitchDegrees);
        if (!keyboardGoesToInterface) {
            CameraInput cameraInput = {};
            cameraInput.turnLeft = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            cameraInput.turnRight = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            cameraInput.moveForward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
            cameraInput.moveBack = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
            cameraInput.moveLeft = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
            cameraInput.moveRight = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
            cameraInput.moveUp = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
            cameraInput.moveDown = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
            cameraInput.fast = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            updateCamera(camera, cameraInput, deltaSeconds);
            // updateCamera 会改动俯仰角，这里把它拉回界面上的取值
            applyCameraPitch(camera, uiState.cameraPitchDegrees);
        }

        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

        LodSceneUniform sceneUniform;
        fillSceneUniform(camera, aspectRatio, uiState.viewMode, ctx.swapchainExtent.width, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.viewMode = uiState.viewMode;
        input.sampler.mipmapped = uiState.mipmapped;
        input.sampler.lodBias = uiState.lodBias;
        input.sampler.maxAnisotropy = uiState.maxAnisotropy;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, sceneUniform, statistics);
        if (!frameDrawn) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.drawCallCount = statistics.drawCallCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);
        if (includeInReport) {
            reportDrawCallCount = statistics.drawCallCount;
        }

        // 按 TimingId 的顺序填满全部计时项，新增计时项时只需要在这里补一行
        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_DRAW] = statistics.cpuRecordDrawMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("view %s | mipmap %s | anisotropy %.0f | lod bias %.2f | draw commands %u\n",
                        viewName(uiState.viewMode), uiState.mipmapped ? "on" : "off",
                        static_cast<double>(uiState.maxAnisotropy), static_cast<double>(uiState.lodBias),
                        uiStatistics.drawCallCount);
            for (int i = 0; i < TIMING_ID_COUNT; ++i) {
                const TimingWindow& window = timingStore.window[i];
                if (i == TIMING_FRAME) {
                    const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
                    std::printf("  %-30s %7.3f +/- %6.3f ms (%.0f FPS)\n",
                                timingStore.items[i].reportColumn, window.mean, window.standardDeviation, fps);
                } else {
                    std::printf("  %-30s %7.3f +/- %6.3f ms\n", timingStore.items[i].reportColumn, window.mean,
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

    std::printf("submitted %llu frames, last frame draw commands %u\n",
                static_cast<unsigned long long>(frameCounter), uiStatistics.drawCallCount);

    if (!reportPath.empty() && !usedSegments) {
        if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }
        const std::string line = reportPrefix(uiState, reportDrawCallCount) + timingReportValueColumns(timingStore);
        appendMeasurementReport(reportPath, reportHeaderColumns, line);
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
