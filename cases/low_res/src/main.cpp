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
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS =
    "ratio,upsample,depth,bilateral,glow,low_res_width,low_res_height,draw_commands";

static bool parseRatio(const std::string& value, uint32_t& ratio)
{
    if (value == "full") {
        ratio = LOWRES_RATIO_FULL;
    } else if (value == "half") {
        ratio = LOWRES_RATIO_HALF;
    } else if (value == "quarter") {
        ratio = LOWRES_RATIO_QUARTER;
    } else {
        return false;
    }
    return true;
}

static bool parseUpsample(const std::string& value, uint32_t& upsample)
{
    if (value == "bilinear") {
        upsample = LOWRES_UPSAMPLE_BILINEAR;
    } else if (value == "bilateral") {
        upsample = LOWRES_UPSAMPLE_BILATERAL;
    } else {
        return false;
    }
    return true;
}

static bool parseDepthMode(const std::string& value, uint32_t& depthMode)
{
    if (value == "copy") {
        depthMode = LOWRES_DEPTH_COPY;
    } else if (value == "subpass") {
        depthMode = LOWRES_DEPTH_SUBPASS;
    } else if (value == "reconstruct") {
        depthMode = LOWRES_DEPTH_RECONSTRUCT;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%.1f,%.2f,%u,%u,%u",
                  lowResRatioName(state.ratio), lowResUpsampleName(state.upsample),
                  lowResDepthModeName(state.depthMode), static_cast<double>(state.bilateralStrength),
                  static_cast<double>(state.glowIntensity), statistics.lowResWidth, statistics.lowResHeight,
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, LowResRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillSceneUniform(const VulkanContext& ctx, const LowResRenderer& renderer, const UiState& state,
                             double sceneTime, LowResSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 30.0f);
    outUniform.viewProjection = projection;
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), static_cast<float>(renderer.lowResWidth),
                  static_cast<float>(renderer.lowResHeight));
    outUniform.modeParams =
        glm::vec4(static_cast<float>(state.ratio), static_cast<float>(state.upsample),
                  static_cast<float>(state.depthMode), state.bilateralStrength);
    outUniform.miscParams = glm::vec4(static_cast<float>(sceneTime), state.glowIntensity, 0.1f, 30.0f);
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
    uint32_t ratio = LOWRES_RATIO_QUARTER;
    uint32_t upsample = LOWRES_UPSAMPLE_BILATERAL;
    uint32_t depthMode = LOWRES_DEPTH_COPY;
    float bilateralStrength = 60.0f;
    float glowIntensity = 1.2f;
    double sceneTime = 0.0;
    bool animate = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ratio") == 0 && i + 1 < argc) {
            if (!parseRatio(argv[i + 1], ratio)) {
                FATAL("--ratio takes full, half or quarter");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--upsample") == 0 && i + 1 < argc) {
            if (!parseUpsample(argv[i + 1], upsample)) {
                FATAL("--upsample takes bilinear or bilateral");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            if (!parseDepthMode(argv[i + 1], depthMode)) {
                FATAL("--depth takes copy, subpass or reconstruct");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--bilateral") == 0 && i + 1 < argc) {
            bilateralStrength = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--glow") == 0 && i + 1 < argc) {
            glowIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            sceneTime = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--freeze") == 0) {
            animate = false;
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

    if (bilateralStrength < 1.0f || bilateralStrength > 400.0f) {
        FATAL("--bilateral takes a value between 1 and 400");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Low Resolution Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    LowResRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.ratio = ratio;
    uiState.upsample = upsample;
    uiState.depthMode = depthMode;
    uiState.bilateralStrength = bilateralStrength;
    uiState.glowIntensity = glowIntensity;

    UiStatistics uiStatistics = {};

    uint64_t frameCounter = 0;
    double previousTime = nowSeconds();
    double lastPrintTime = previousTime;
    const double startTime = previousTime;

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

        if (verb == "ratio") {
            std::string value;
            if (!(stream >> value) || !parseRatio(value, uiState.ratio)) {
                return "err: ratio takes full, half or quarter";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "upsample") {
            std::string value;
            if (!(stream >> value) || !parseUpsample(value, uiState.upsample)) {
                return "err: upsample takes bilinear or bilateral";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "depth") {
            std::string value;
            if (!(stream >> value) || !parseDepthMode(value, uiState.depthMode)) {
                return "err: depth takes copy, subpass or reconstruct";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "bilateral") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 400.0) {
                return "err: bilateral out of range";
            }
            uiState.bilateralStrength = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "glow") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 4.0) {
                return "err: glow out of range";
            }
            uiState.glowIntensity = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "time") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: time takes seconds";
            }
            sceneTime = value;
            animate = false;
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
        if (animate) {
            sceneTime += static_cast<double>(deltaSeconds);
        }

        LowResSceneUniform sceneUniform;
        fillSceneUniform(ctx, renderer, uiState, sceneTime, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.ratio = uiState.ratio;
        input.options.upsample = uiState.upsample;
        input.options.depthMode = uiState.depthMode;
        input.options.bilateralStrength = uiState.bilateralStrength;
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
        uiStatistics.lowResWidth = statistics.lowResWidth;
        uiStatistics.lowResHeight = statistics.lowResHeight;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
        timingValues[TIMING_CPU_RECORD_LOWRES] = statistics.cpuRecordLowResMilliseconds;
        timingValues[TIMING_CPU_RECORD_COMPOSITE] = statistics.cpuRecordCompositeMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("ratio %s | upsample %s | depth %s | low res %ux%u | draw commands %u\n",
                        lowResRatioName(uiState.ratio), lowResUpsampleName(uiState.upsample),
                        lowResDepthModeName(uiState.depthMode), uiStatistics.lowResWidth,
                        uiStatistics.lowResHeight, uiStatistics.drawCallCount);
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

    std::printf("submitted %llu frames\n", static_cast<unsigned long long>(frameCounter));

    if (!reportPath.empty() && !usedSegments) {
        if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }
        const std::string line = reportPrefix(uiState, uiStatistics) +
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
