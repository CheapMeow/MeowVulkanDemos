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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

static const char* const REPORT_HEADER_COLUMNS =
    "threshold,chain,levels,threshold_value,intensity,order,render_passes,draw_commands";

static bool parseThreshold(const std::string& value, uint32_t& threshold)
{
    if (value == "none") {
        threshold = BLOOM_THRESHOLD_NONE;
    } else if (value == "hard") {
        threshold = BLOOM_THRESHOLD_HARD;
    } else if (value == "soft") {
        threshold = BLOOM_THRESHOLD_SOFT;
    } else {
        return false;
    }
    return true;
}

static bool parseChain(const std::string& value, uint32_t& chain)
{
    if (value == "gaussian") {
        chain = BLOOM_CHAIN_GAUSSIAN;
    } else if (value == "kawase") {
        chain = BLOOM_CHAIN_KAWASE;
    } else if (value == "multi") {
        chain = BLOOM_CHAIN_MULTI;
    } else {
        return false;
    }
    return true;
}

static bool parseOrder(const std::string& value, uint32_t& order)
{
    if (value == "aa_first") {
        order = BLOOM_ORDER_AA_FIRST;
    } else if (value == "tonemap_first") {
        order = BLOOM_ORDER_TONEMAP_FIRST;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%u,%.2f,%.2f,%s,%u,%u", bloomThresholdName(state.threshold),
                  bloomChainName(state.chain), state.levels,
                  static_cast<double>(state.thresholdValue), static_cast<double>(state.intensity),
                  bloomOrderName(state.order), statistics.renderPassCount, statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, BloomRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillUniform(const VulkanContext& ctx, const UiState& state, double sceneTime,
                        BloomSceneUniform& outUniform)
{
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.modeParams =
        glm::vec4(static_cast<float>(state.threshold), static_cast<float>(state.chain),
                  static_cast<float>(state.levels), static_cast<float>(state.order));
    outUniform.miscParams = glm::vec4(state.thresholdValue, state.intensity,
                                      static_cast<float>(sceneTime), 0.0f);
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
    uint32_t threshold = BLOOM_THRESHOLD_HARD;
    uint32_t chain = BLOOM_CHAIN_GAUSSIAN;
    uint32_t order = BLOOM_ORDER_AA_FIRST;
    uint32_t levels = 4;
    float thresholdValue = 1.5f;
    float intensity = 0.6f;
    double sceneTime = 0.0;
    bool animate = true;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            if (!parseThreshold(argv[i + 1], threshold)) {
                FATAL("--threshold takes none, hard or soft");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--chain") == 0 && i + 1 < argc) {
            if (!parseChain(argv[i + 1], chain)) {
                FATAL("--chain takes gaussian, kawase or multi");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--order") == 0 && i + 1 < argc) {
            if (!parseOrder(argv[i + 1], order)) {
                FATAL("--order takes aa_first or tonemap_first");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--levels") == 0 && i + 1 < argc) {
            levels = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--threshold-value") == 0 && i + 1 < argc) {
            thresholdValue = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--intensity") == 0 && i + 1 < argc) {
            intensity = static_cast<float>(std::atof(argv[i + 1]));
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

    if (levels < 1 || levels > 5) {
        FATAL("--levels takes a value between 1 and 5");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Bloom Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    BloomRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.threshold = threshold;
    uiState.chain = chain;
    uiState.order = order;
    uiState.levels = levels;
    uiState.thresholdValue = thresholdValue;
    uiState.intensity = intensity;

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

        if (verb == "threshold") {
            std::string value;
            if (!(stream >> value) || !parseThreshold(value, uiState.threshold)) {
                return "err: threshold takes none, hard or soft";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "chain") {
            std::string value;
            if (!(stream >> value) || !parseChain(value, uiState.chain)) {
                return "err: chain takes gaussian, kawase or multi";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "order") {
            std::string value;
            if (!(stream >> value) || !parseOrder(value, uiState.order)) {
                return "err: order takes aa_first or tonemap_first";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "levels") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 5) {
                return "err: levels takes a value between 1 and 5";
            }
            uiState.levels = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "threshold-value" || verb == "intensity") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0) {
                return "err: " + verb + " takes a non-negative value";
            }
            if (verb == "threshold-value") {
                uiState.thresholdValue = static_cast<float>(value);
            } else {
                uiState.intensity = static_cast<float>(value);
            }
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

        BloomSceneUniform uniform;
        fillUniform(ctx, uiState, sceneTime, uniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.threshold = uiState.threshold;
        input.options.chain = uiState.chain;
        input.options.order = uiState.order;
        input.options.levels = uiState.levels;
        input.options.thresholdValue = uiState.thresholdValue;
        input.options.intensity = uiState.intensity;
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
        uiStatistics.renderPassCount = statistics.renderPassCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
        timingValues[TIMING_CPU_RECORD_CHAIN] = statistics.cpuRecordChainMilliseconds;
        timingValues[TIMING_CPU_RECORD_PRESENT] = statistics.cpuRecordPresentMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("threshold %s | chain %s | levels %u | order %s | passes %u | draw commands %u\n",
                        bloomThresholdName(uiState.threshold), bloomChainName(uiState.chain), uiState.levels,
                        bloomOrderName(uiState.order), uiStatistics.renderPassCount,
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
