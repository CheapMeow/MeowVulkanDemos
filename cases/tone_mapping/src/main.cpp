#include "asset_file.h"
#include "case_ui.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
#include "renderer.h"
#include "scene.h"
#include "scene_setup.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS =
    "operator,channel,encoding,exposure_ev,white_point,draw_commands";

static bool parseOperator(const std::string& value, uint32_t& op)
{
    if (value == "none") {
        op = TONE_MAP_OP_NONE;
    } else if (value == "reinhard") {
        op = TONE_MAP_OP_REINHARD;
    } else if (value == "reinhard_extended") {
        op = TONE_MAP_OP_REINHARD_EXTENDED;
    } else if (value == "aces") {
        op = TONE_MAP_OP_ACES;
    } else if (value == "filmic") {
        op = TONE_MAP_OP_FILMIC;
    } else {
        return false;
    }
    return true;
}

static bool parseChannel(const std::string& value, uint32_t& channel)
{
    if (value == "per_channel") {
        channel = TONE_MAP_CHANNEL_PER_CHANNEL;
    } else if (value == "luminance") {
        channel = TONE_MAP_CHANNEL_LUMINANCE;
    } else {
        return false;
    }
    return true;
}

static bool parseEncoding(const std::string& value, uint32_t& encoding)
{
    if (value == "linear") {
        encoding = TONE_MAP_ENCODING_LINEAR;
    } else if (value == "gamma") {
        encoding = TONE_MAP_ENCODING_GAMMA;
    } else if (value == "srgb") {
        encoding = TONE_MAP_ENCODING_SRGB;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%.2f,%.2f,%u", toneMapOperatorName(state.toneMapOperator),
                  toneMapChannelName(state.toneMapChannel), toneMapEncodingName(state.outputEncoding),
                  state.exposureEv, state.whitePoint, drawCommands);
    return std::string(prefix);
}

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建
static void rebuildSwapchainResources(VulkanContext& ctx, ToneMapRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillUniform(const Camera& camera, float aspectRatio, const UiState& state,
                        ToneMapUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.inverseViewProjection = glm::inverse(matrices.viewProjection);
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.lightDirection = glm::vec4(glm::normalize(glm::vec3(0.45f, 0.75f, 0.35f)),
                                          state.lightIntensity);
    outUniform.sunDirection = glm::vec4(glm::normalize(glm::vec3(0.35f, 0.30f, -0.88f)),
                                        state.sunIntensity);
    outUniform.skyParams = glm::vec4(state.skyIntensity, 1.6f, 0.0f, 0.0f);
    outUniform.operatorParams = glm::vec4(static_cast<float>(state.toneMapOperator),
                                          static_cast<float>(state.toneMapChannel),
                                          std::exp2(state.exposureEv), state.whitePoint);
    outUniform.encodingParams = glm::vec4(static_cast<float>(state.outputEncoding), 0.0f, 0.0f, 0.0f);
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

    UiState uiState = {};
    uiState.cameraMoveSpeed = 3.0f;
    uiState.toneMapOperator = TONE_MAP_OP_ACES;
    uiState.toneMapChannel = TONE_MAP_CHANNEL_PER_CHANNEL;
    uiState.outputEncoding = TONE_MAP_ENCODING_SRGB;
    uiState.exposureEv = 0.0f;
    uiState.whitePoint = 4.0f;
    uiState.lightIntensity = 5.0f;
    uiState.skyIntensity = 1.2f;
    uiState.sunIntensity = 900.0f;

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
        } else if (std::strcmp(argv[i], "--op") == 0 && i + 1 < argc) {
            if (!parseOperator(argv[i + 1], uiState.toneMapOperator)) {
                FATAL("--op takes none, reinhard, reinhard_extended, aces or filmic");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            if (!parseChannel(argv[i + 1], uiState.toneMapChannel)) {
                FATAL("--channel takes per_channel or luminance");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--encoding") == 0 && i + 1 < argc) {
            if (!parseEncoding(argv[i + 1], uiState.outputEncoding)) {
                FATAL("--encoding takes linear, gamma or srgb");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            uiState.exposureEv = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--white-point") == 0 && i + 1 < argc) {
            uiState.whitePoint = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--light") == 0 && i + 1 < argc) {
            uiState.lightIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--sky") == 0 && i + 1 < argc) {
            uiState.skyIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--sun") == 0 && i + 1 < argc) {
            uiState.sunIntensity = static_cast<float>(std::atof(argv[i + 1]));
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Tone Mapping Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    MeshData objectMesh;
    loadObjFromMemory(readAssetBytes("backpack/backpack.obj"), objectMesh);

    MeshData barMesh;
    buildReferenceBarMesh(referenceBarValues(), barMesh);

    ToneMapRenderer renderer = {};
    createRenderer(ctx, renderer, objectMesh, barMesh);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    Camera camera = {};
    camera.position = glm::vec3(0.0f, 0.9f, 3.2f);
    camera.yaw = 0.0f;
    camera.pitch = -0.07f;
    camera.verticalFieldOfView = glm::radians(60.0f);
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.0f;
    camera.moveSpeed = uiState.cameraMoveSpeed;

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

        if (verb == "op") {
            std::string value;
            if (!(stream >> value) || !parseOperator(value, uiState.toneMapOperator)) {
                return "err: op takes none, reinhard, reinhard_extended, aces or filmic";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "channel") {
            std::string value;
            if (!(stream >> value) || !parseChannel(value, uiState.toneMapChannel)) {
                return "err: channel takes per_channel or luminance";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "encoding") {
            std::string value;
            if (!(stream >> value) || !parseEncoding(value, uiState.outputEncoding)) {
                return "err: encoding takes linear, gamma or srgb";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "exposure") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: exposure takes a number";
            }
            uiState.exposureEv = value;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "white-point") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: white-point takes a number";
            }
            uiState.whitePoint = value;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "light" || verb == "sky" || verb == "sun") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "light") {
                uiState.lightIntensity = value;
            } else if (verb == "sky") {
                uiState.skyIntensity = value;
            } else {
                uiState.sunIntensity = value;
            }
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
            const std::string line = reportPrefix(uiState, reportDrawCallCount) +
                                     timingReportValueColumns(timingStore);
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

        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

        ToneMapUniform uniform;
        fillUniform(camera, aspectRatio, uiState, uniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput frameInput = {};
        frameInput.drawUserInterface = interfaceEnabled;
        frameInput.options.op = uiState.toneMapOperator;
        frameInput.options.channel = uiState.toneMapChannel;
        frameInput.options.encoding = uiState.outputEncoding;
        frameInput.options.exposureEv = uiState.exposureEv;
        frameInput.options.whitePoint = uiState.whitePoint;
        frameInput.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, frameInput, uniform, statistics);
        if (!frameDrawn) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.drawCallCount = statistics.drawCallCount;
        uiStatistics.renderPassCount = statistics.renderPassCount;

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
        timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
        timingValues[TIMING_CPU_RECORD_TONEMAP] = statistics.cpuRecordTonemapMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        timingValues[TIMING_GPU_SCENE] = statistics.gpuSceneMilliseconds;
        timingValues[TIMING_GPU_TONEMAP] = statistics.gpuTonemapMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("op %s | channel %s | encoding %s | EV %.2f | white %.2f | draw commands %u\n",
                        toneMapOperatorName(uiState.toneMapOperator),
                        toneMapChannelName(uiState.toneMapChannel),
                        toneMapEncodingName(uiState.outputEncoding), uiState.exposureEv, uiState.whitePoint,
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
        const std::string line = reportPrefix(uiState, reportDrawCallCount) +
                                 timingReportValueColumns(timingStore);
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
