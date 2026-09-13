#include "case_ui.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "renderer.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

static const float NEAR_PLANE = 1.0f;
static const float FAR_PLANE = 25.0f;
static const float CAMERA_DISTANCE = 4.2f;
static const glm::vec3 CAMERA_TARGET(0.0f, 0.0f, -2.2f);

static const char* const REPORT_HEADER_COLUMNS =
    "culling,extreme,level_mode,level,depth_source,expansion,visible,culled,draw_commands";

static bool parseExtreme(const std::string& value, uint32_t& extreme)
{
    if (value == "farthest") {
        extreme = 0;
    } else if (value == "nearest") {
        extreme = 1;
    } else {
        return false;
    }
    return true;
}

static bool parseLevelMode(const std::string& value, uint32_t& levelMode)
{
    if (value == "fixed") {
        levelMode = 0;
    } else if (value == "auto") {
        levelMode = 1;
    } else {
        return false;
    }
    return true;
}

static bool parseDepthSource(const std::string& value, bool& currentFrameDepth)
{
    if (value == "previous") {
        currentFrameDepth = false;
    } else if (value == "current") {
        currentFrameDepth = true;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[288];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%u,%s,%.2f,%u,%u,%u",
                  state.occlusionCulling ? "on" : "off", hzbExtremeName(state.pyramidExtreme),
                  hzbLevelModeName(state.levelMode), state.level,
                  hzbDepthSourceName(state.currentFrameDepth), state.expansion,
                  statistics.visibleInstanceCount, statistics.culledInstanceCount,
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, HzbRenderer& renderer,
                                      GpuBuffer& captureBuffer, bool capturePending)
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

static void fillUniform(const VulkanContext& ctx, const UiState& state, uint32_t instanceCount,
                        HzbSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const float yaw = glm::radians(state.yawDegrees);
    const glm::vec3 eye(std::sin(yaw) * CAMERA_DISTANCE, 0.35f, std::cos(yaw) * CAMERA_DISTANCE);
    outUniform.viewProjection = glm::perspective(glm::radians(60.0f), aspect, NEAR_PLANE, FAR_PLANE) *
                                glm::lookAt(eye, CAMERA_TARGET, glm::vec3(0.0f, 1.0f, 0.0f));
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), state.expansion,
                  state.levelMode == 0 ? 0.0f : 1.0f);
    outUniform.modeParams = glm::vec4(state.occlusionCulling ? 1.0f : 0.0f,
                                      static_cast<float>(state.pyramidExtreme),
                                      static_cast<float>(state.level), NEAR_PLANE);
    outUniform.miscParams = glm::vec4(static_cast<float>(instanceCount), state.visualize ? 1.0f : 0.0f,
                                      FAR_PLANE, state.currentFrameDepth ? 1.0f : 0.0f);
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
    uiState.occlusionCulling = true;
    uiState.pyramidExtreme = 0;
    uiState.levelMode = 0;
    uiState.level = 0;
    uiState.currentFrameDepth = false;
    uiState.expansion = 1.0f;
    uiState.visualize = false;
    uiState.yawDegrees = 0.0f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-culling") == 0) {
            uiState.occlusionCulling = false;
        } else if (std::strcmp(argv[i], "--culling") == 0) {
            uiState.occlusionCulling = true;
        } else if (std::strcmp(argv[i], "--extreme") == 0 && i + 1 < argc) {
            if (!parseExtreme(argv[i + 1], uiState.pyramidExtreme)) {
                FATAL("--extreme takes farthest or nearest");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--level-mode") == 0 && i + 1 < argc) {
            if (!parseLevelMode(argv[i + 1], uiState.levelMode)) {
                FATAL("--level-mode takes fixed or auto");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            uiState.level = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--depth-source") == 0 && i + 1 < argc) {
            if (!parseDepthSource(argv[i + 1], uiState.currentFrameDepth)) {
                FATAL("--depth-source takes previous or current");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--expansion") == 0 && i + 1 < argc) {
            uiState.expansion = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--visualize") == 0) {
            uiState.visualize = true;
        } else if (std::strcmp(argv[i], "--yaw") == 0 && i + 1 < argc) {
            uiState.yawDegrees = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
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

    if (uiState.level >= HZB_LEVEL_COUNT) {
        FATAL("--level takes a value between 0 and %d", static_cast<int>(HZB_LEVEL_COUNT) - 1);
    }
    if (uiState.expansion < 1.0f || uiState.expansion > 2.0f) {
        FATAL("--expansion takes a value between 1.0 and 2.0");
    }

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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan HZB Occlusion Culling Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    HzbRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiStatistics uiStatistics = {};
    uiStatistics.instanceCount = renderer.instanceCount;

    uint64_t frameCounter = 0;
    const double startTime = nowSeconds();
    double previousTime = startTime;
    double lastPrintTime = startTime;

    TimingStore timingStore;
    initTimingStore(timingStore, caseTimingItems(), TIMING_ID_COUNT, startTime);
    const std::string reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(timingStore);

    const double warmUpSeconds = 1.5;

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

        if (verb == "culling") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: culling takes on or off";
            }
            uiState.occlusionCulling = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "extreme") {
            std::string value;
            if (!(stream >> value) || !parseExtreme(value, uiState.pyramidExtreme)) {
                return "err: extreme takes farthest or nearest";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "level-mode") {
            std::string value;
            if (!(stream >> value) || !parseLevelMode(value, uiState.levelMode)) {
                return "err: level-mode takes fixed or auto";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "level") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value >= static_cast<long>(HZB_LEVEL_COUNT)) {
                return "err: level out of range";
            }
            uiState.level = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "depth-source") {
            std::string value;
            if (!(stream >> value) || !parseDepthSource(value, uiState.currentFrameDepth)) {
                return "err: depth-source takes previous or current";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "expansion") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 2.0) {
                return "err: expansion takes a value between 1.0 and 2.0";
            }
            uiState.expansion = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "visualize") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: visualize takes on or off";
            }
            uiState.visualize = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "yaw") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: yaw takes degrees";
            }
            uiState.yawDegrees = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "begin") {
            usedSegments = true;
            segmentActive = true;
            segmentStartSeconds = nowSeconds();
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
                reportPrefix(uiState, uiStatistics) + timingReportValueColumns(timingStore);
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

        HzbSceneUniform uniform;
        fillUniform(ctx, uiState, renderer.instanceCount, uniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, uniform, statistics);
        if (!frameDrawn) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.drawCallCount = statistics.drawCallCount;
        uiStatistics.visibleInstanceCount = statistics.visibleInstanceCount;
        uiStatistics.culledInstanceCount = statistics.culledInstanceCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_PREPASS] = statistics.cpuRecordPrepassMilliseconds;
        timingValues[TIMING_CPU_RECORD_CULL] = statistics.cpuRecordCullMilliseconds;
        timingValues[TIMING_CPU_RECORD_PYRAMID] = statistics.cpuRecordPyramidMilliseconds;
        timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
        timingValues[TIMING_CPU_RECORD_PRESENT] = statistics.cpuRecordPresentMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        timingValues[TIMING_GPU_PREPASS] = statistics.gpuPrepassMilliseconds;
        timingValues[TIMING_GPU_CULL] = statistics.gpuCullMilliseconds;
        timingValues[TIMING_GPU_PYRAMID] = statistics.gpuPyramidMilliseconds;
        timingValues[TIMING_GPU_SCENE] = statistics.gpuSceneMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("culling %s | extreme %s | level %s %u | depth %s | expansion %.2f | "
                        "visible %u | culled %u | draws %u\n",
                        uiState.occlusionCulling ? "on" : "off", hzbExtremeName(uiState.pyramidExtreme),
                        hzbLevelModeName(uiState.levelMode), uiState.level,
                        hzbDepthSourceName(uiState.currentFrameDepth), uiState.expansion,
                        uiStatistics.visibleInstanceCount, uiStatistics.culledInstanceCount,
                        uiStatistics.drawCallCount);
            for (int i = 0; i < TIMING_ID_COUNT; ++i) {
                const TimingWindow& timingWindow = timingStore.window[i];
                if (i == TIMING_FRAME) {
                    const double fps = timingWindow.mean > 0.0 ? 1000.0 / timingWindow.mean : 0.0;
                    std::printf("  %-30s %7.3f +/- %6.3f ms (%.0f FPS)\n",
                                timingStore.items[i].reportColumn, timingWindow.mean,
                                timingWindow.standardDeviation, fps);
                } else {
                    std::printf("  %-30s %7.3f +/- %6.3f ms\n", timingStore.items[i].reportColumn,
                                timingWindow.mean, timingWindow.standardDeviation);
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

    std::printf("submitted %llu frames\n", static_cast<unsigned long long>(frameCounter));

    if (!reportPath.empty() && !usedSegments) {
        if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }
        const std::string line =
            reportPrefix(uiState, uiStatistics) + timingReportValueColumns(timingStore);
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
