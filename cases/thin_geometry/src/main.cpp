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
static const char* const REPORT_HEADER_COLUMNS =
    "mode,rod_count,rod_width,pan,fxaa_steps,fxaa_threshold,draw_commands";

static const char* modeArgumentName(uint32_t mode)
{
    if (mode == ANTIALIAS_MSAA) {
        return "msaa";
    }
    if (mode == ANTIALIAS_FXAA) {
        return "fxaa";
    }
    return "none";
}

// 多重采样方式用四倍采样，其余方式用单采样
static uint32_t requestedSampleCount(uint32_t mode)
{
    return mode == ANTIALIAS_MSAA ? 4 : 1;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%u,%.3f,%.2f,%u,%.3f,%u", modeArgumentName(state.mode),
                  state.rodCount, state.rodWidthPixels, state.panOffsetPixels, state.fxaaSearchSteps,
                  state.fxaaEdgeThreshold, statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, ThinRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillSceneUniform(const VulkanContext& ctx, const UiState& state, ThinSceneUniform& outUniform)
{
    outUniform.viewportParams = glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                                          static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.colorParams = glm::vec4(state.backgroundIntensity, state.backgroundIntensity,
                                       state.backgroundIntensity, 0.0f);
    outUniform.modeParams = glm::vec4(state.panOffsetPixels, 0.0f, 0.0f, 0.0f);
    outUniform.params = glm::vec4(state.rodWidthPixels, 0.0f, 0.0f, 0.0f);
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
    uint32_t mode = ANTIALIAS_MSAA;
    uint32_t rodCount = 64;
    float rodWidthPixels = 1.5f;
    float panSpeed = 40.0f;
    double panOffsetPixels = 0.0;
    float backgroundIntensity = 0.05f;
    uint32_t fxaaSearchSteps = 8;
    float fxaaEdgeThreshold = 0.125f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* value = argv[i + 1];
            if (std::strcmp(value, "none") == 0) {
                mode = ANTIALIAS_NONE;
            } else if (std::strcmp(value, "msaa") == 0) {
                mode = ANTIALIAS_MSAA;
            } else if (std::strcmp(value, "fxaa") == 0) {
                mode = ANTIALIAS_FXAA;
            } else {
                FATAL("--mode takes none, msaa or fxaa");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--rod-count") == 0 && i + 1 < argc) {
            rodCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--rod-width") == 0 && i + 1 < argc) {
            rodWidthPixels = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--pan-speed") == 0 && i + 1 < argc) {
            panSpeed = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--pan") == 0 && i + 1 < argc) {
            panOffsetPixels = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--background") == 0 && i + 1 < argc) {
            backgroundIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--fxaa-steps") == 0 && i + 1 < argc) {
            fxaaSearchSteps = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--fxaa-threshold") == 0 && i + 1 < argc) {
            fxaaEdgeThreshold = static_cast<float>(std::atof(argv[i + 1]));
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

    if (rodCount < 1 || rodCount > 512) {
        FATAL("--rod-count takes a value between 1 and 512");
    }
    if (rodWidthPixels <= 0.0f) {
        FATAL("--rod-width takes a positive pixel count");
    }
    if (fxaaSearchSteps < 1 || fxaaSearchSteps > 16) {
        FATAL("--fxaa-steps takes a value between 1 and 16");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Thin Geometry Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    // 采样数不能超过设备支持的上限
    uint32_t sampleCount = requestedSampleCount(mode);
    const VkSampleCountFlags supported =
        ctx.physicalDeviceProperties.limits.framebufferColorSampleCounts &
        ctx.physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    while (sampleCount > 1) {
        const VkSampleCountFlagBits flag = sampleCount >= 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
        if ((supported & flag) != 0) {
            break;
        }
        sampleCount /= 2;
    }

    ThinRenderer renderer = {};
    createRenderer(ctx, renderer, sampleCount);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.mode = mode;
    uiState.rodCount = rodCount;
    uiState.rodWidthPixels = rodWidthPixels;
    uiState.panSpeed = panSpeed;
    uiState.panOffsetPixels = static_cast<float>(panOffsetPixels);
    uiState.backgroundIntensity = backgroundIntensity;
    uiState.fxaaSearchSteps = fxaaSearchSteps;
    uiState.fxaaEdgeThreshold = fxaaEdgeThreshold;

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

        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || (value != "none" && value != "msaa" && value != "fxaa")) {
                return "err: mode takes none, msaa or fxaa";
            }
            uiState.mode = value == "none" ? ANTIALIAS_NONE
                          : value == "msaa" ? ANTIALIAS_MSAA
                                            : ANTIALIAS_FXAA;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "rod-count") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 512) {
                return "err: rod-count takes a value between 1 and 512";
            }
            uiState.rodCount = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "rod-width") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: rod-width takes a positive pixel count";
            }
            uiState.rodWidthPixels = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "pan-speed") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0) {
                return "err: pan-speed takes a non-negative value";
            }
            uiState.panSpeed = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "pan") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: pan takes a pixel offset";
            }
            panOffsetPixels = value;
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
        if (verb == "fxaa-steps") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 16) {
                return "err: fxaa-steps takes a value between 1 and 16";
            }
            uiState.fxaaSearchSteps = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "fxaa-threshold") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0 || value > 1.0) {
                return "err: fxaa-threshold out of range";
            }
            uiState.fxaaEdgeThreshold = static_cast<float>(value);
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
        panOffsetPixels += static_cast<double>(uiState.panSpeed) * deltaSeconds;
        uiState.panOffsetPixels = static_cast<float>(panOffsetPixels);

        ThinSceneUniform sceneUniform;
        fillSceneUniform(ctx, uiState, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.mode = uiState.mode;
        // 只有多重采样方式用多采样附件，其余方式都是单采样
        input.options.sampleCount = uiState.mode == ANTIALIAS_MSAA ? sampleCount : 1;
        input.options.rodCount = uiState.rodCount;
        input.options.fxaaSearchSteps = uiState.fxaaSearchSteps;
        input.options.fxaaEdgeThreshold = uiState.fxaaEdgeThreshold;
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

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_GEOMETRY] = statistics.cpuRecordGeometryPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_ANTIALIAS] = statistics.cpuRecordAntialiasMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("mode %s | rods %u | width %.2f px | draw commands %u\n",
                        modeArgumentName(uiState.mode), uiState.rodCount, uiState.rodWidthPixels,
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
