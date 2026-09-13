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

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS = "mode,samples,threshold,fragment_count,draw_commands";

// alpha to coverage 需要采样数大于一，单采样下三种方式里只有前两种成立
static uint32_t effectiveMode(uint32_t mode, uint32_t sampleCount)
{
    if (mode == ALPHA_MODE_TO_COVERAGE && sampleCount == 1) {
        return ALPHA_MODE_TEST;
    }
    return mode;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%u,%.3f,%u,%u",
                  alphaModeName(effectiveMode(state.mode, state.sampleCount)), state.sampleCount,
                  state.threshold, statistics.fragmentCount, statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, AlphaRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillSceneUniform(const VulkanContext& ctx, const UiState& state, float accumulatedAngle,
                             AlphaSceneUniform& outUniform)
{
    outUniform.viewportParams = glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                                          static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.colorParams = glm::vec4(state.backgroundIntensity, state.backgroundIntensity,
                                       state.backgroundIntensity, 0.0f);
    outUniform.modeParams = glm::vec4(static_cast<float>(effectiveMode(state.mode, state.sampleCount)),
                                      state.threshold, 0.0f, 0.0f);
    outUniform.animationParams = glm::vec4(accumulatedAngle, 0.0f, 0.0f, 0.0f);
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
    uint32_t sampleCount = 4;
    uint32_t mode = ALPHA_MODE_TO_COVERAGE;
    float threshold = 0.5f;
    float backgroundIntensity = 0.06f;
    float rotationSpeed = 0.6f;
    double accumulatedAngle = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            sampleCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* value = argv[i + 1];
            if (std::strcmp(value, "test") == 0) {
                mode = ALPHA_MODE_TEST;
            } else if (std::strcmp(value, "blend") == 0) {
                mode = ALPHA_MODE_BLEND;
            } else if (std::strcmp(value, "coverage") == 0) {
                mode = ALPHA_MODE_TO_COVERAGE;
            } else {
                FATAL("--mode takes test, blend or coverage");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--background") == 0 && i + 1 < argc) {
            backgroundIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--rotation-speed") == 0 && i + 1 < argc) {
            rotationSpeed = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--angle") == 0 && i + 1 < argc) {
            accumulatedAngle = std::atof(argv[i + 1]);
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

    if (sampleCount != 1 && sampleCount != 2 && sampleCount != 4 && sampleCount != 8) {
        FATAL("--samples takes 1, 2, 4 or 8");
    }
    if (threshold < 0.0f || threshold > 1.0f) {
        FATAL("--threshold takes a value between 0 and 1");
    }
    if (backgroundIntensity < 0.0f || backgroundIntensity > 1.0f) {
        FATAL("--background takes a value between 0 and 1");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Alpha Modes Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    // 采样数不能超过设备支持的上限
    const VkSampleCountFlags supported =
        ctx.physicalDeviceProperties.limits.framebufferColorSampleCounts &
        ctx.physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    while (sampleCount > 1) {
        const VkSampleCountFlagBits flag = sampleCount >= 8   ? VK_SAMPLE_COUNT_8_BIT
                                           : sampleCount >= 4 ? VK_SAMPLE_COUNT_4_BIT
                                           : sampleCount >= 2 ? VK_SAMPLE_COUNT_2_BIT
                                                              : VK_SAMPLE_COUNT_1_BIT;
        if ((supported & flag) != 0) {
            break;
        }
        sampleCount /= 2;
    }

    AlphaRenderer renderer = {};
    createRenderer(ctx, renderer, sampleCount);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.tonemapRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.mode = mode;
    uiState.sampleCount = sampleCount;
    uiState.threshold = threshold;
    uiState.backgroundIntensity = backgroundIntensity;
    uiState.rotationSpeed = rotationSpeed;

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

        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || (value != 1 && value != 2 && value != 4 && value != 8)) {
                return "err: samples takes 1, 2, 4 or 8";
            }
            uiState.sampleCount = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || (value != "test" && value != "blend" && value != "coverage")) {
                return "err: mode takes test, blend or coverage";
            }
            uiState.mode = value == "test" ? ALPHA_MODE_TEST
                          : value == "blend" ? ALPHA_MODE_BLEND
                                             : ALPHA_MODE_TO_COVERAGE;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "threshold") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: threshold out of range";
            }
            uiState.threshold = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "background") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: background out of range";
            }
            uiState.backgroundIntensity = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "rotation-speed") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 2.0) {
                return "err: rotation-speed out of range";
            }
            uiState.rotationSpeed = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "angle") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: angle takes a value in radians";
            }
            accumulatedAngle = value;
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
        accumulatedAngle += static_cast<double>(uiState.rotationSpeed) * deltaSeconds;

        AlphaSceneUniform sceneUniform;
        fillSceneUniform(ctx, uiState, static_cast<float>(accumulatedAngle), sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.mode = effectiveMode(uiState.mode, uiState.sampleCount);
        input.options.sampleCount = uiState.sampleCount;
        input.options.threshold = uiState.threshold;
        input.options.accumulatedAngle = static_cast<float>(accumulatedAngle);
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
        uiStatistics.fragmentCount = statistics.fragmentCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_GEOMETRY] = statistics.cpuRecordGeometryPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_RESOLVE] = statistics.cpuRecordResolveMilliseconds;
        timingValues[TIMING_CPU_RECORD_TONEMAP] = statistics.cpuRecordTonemapMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("mode %s | samples %u | fragment calls %u | draw commands %u\n",
                        alphaModeName(effectiveMode(uiState.mode, uiState.sampleCount)), uiState.sampleCount,
                        uiStatistics.fragmentCount, uiStatistics.drawCallCount);
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

    std::printf("submitted %llu frames, last frame fragment calls %u\n",
                static_cast<unsigned long long>(frameCounter), uiStatistics.fragmentCount);

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
