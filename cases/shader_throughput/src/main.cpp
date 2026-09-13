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
    "kernel,precision,iterations,samples,divergent,workgroups,draw_commands";

static bool parseKernel(const std::string& value, uint32_t& kernel)
{
    if (value == "alu") {
        kernel = THROUGHPUT_ALU;
    } else if (value == "sample") {
        kernel = THROUGHPUT_SAMPLE;
    } else if (value == "branch") {
        kernel = THROUGHPUT_BRANCH;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%u,%u,%d,%u,%u", throughputKernelName(state.kernel),
                  throughputPrecisionName(state.precision), state.iterations, state.samples,
                  state.divergent ? 1 : 0, statistics.workgroupCount, statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, ThroughputRenderer& renderer,
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

static void fillUniform(const VulkanContext& ctx, const UiState& state, ThroughputUniform& outUniform)
{
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.kernelParams = glm::vec4(static_cast<float>(state.kernel),
                                        static_cast<float>(state.iterations),
                                        static_cast<float>(state.samples),
                                        state.randomLocality ? 1.0f : 0.0f);
    outUniform.miscParams = glm::vec4(state.divergent ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
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
    uint32_t kernel = THROUGHPUT_ALU;
    uint32_t precision = THROUGHPUT_HIGH;
    uint32_t iterations = 32;
    uint32_t samples = 8;
    bool divergent = false;
    bool randomLocality = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
            if (!parseKernel(argv[i + 1], kernel)) {
                FATAL("--kernel takes alu, sample or branch");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--precision") == 0 && i + 1 < argc) {
            if (std::strcmp(argv[i + 1], "medium") == 0) {
                precision = THROUGHPUT_MEDIUM;
            } else if (std::strcmp(argv[i + 1], "high") == 0) {
                precision = THROUGHPUT_HIGH;
            } else {
                FATAL("--precision takes high or medium");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            samples = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--divergent") == 0) {
            divergent = true;
        } else if (std::strcmp(argv[i], "--random-locality") == 0) {
            randomLocality = true;
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

    if (iterations < 1 || iterations > 256) {
        FATAL("--iterations takes a value between 1 and 256");
    }
    if (samples < 1 || samples > 64) {
        FATAL("--samples takes a value between 1 and 64");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Shader Throughput Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    ThroughputRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.kernel = kernel;
    uiState.precision = precision;
    uiState.iterations = iterations;
    uiState.samples = samples;
    uiState.divergent = divergent;
    uiState.randomLocality = randomLocality;

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

        if (verb == "kernel") {
            std::string value;
            if (!(stream >> value) || !parseKernel(value, uiState.kernel)) {
                return "err: kernel takes alu, sample or branch";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "precision") {
            std::string value;
            if (!(stream >> value) || (value != "high" && value != "medium")) {
                return "err: precision takes high or medium";
            }
            uiState.precision = value == "medium" ? THROUGHPUT_MEDIUM : THROUGHPUT_HIGH;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "iterations" || verb == "samples") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 256) {
                return "err: " + verb + " out of range";
            }
            if (verb == "iterations") {
                uiState.iterations = static_cast<uint32_t>(value);
            } else {
                uiState.samples = static_cast<uint32_t>(value);
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "divergent" || verb == "random-locality") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: " + verb + " takes 0 or 1";
            }
            if (verb == "divergent") {
                uiState.divergent = value == 1;
            } else {
                uiState.randomLocality = value == 1;
            }
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

        ThroughputUniform uniform;
        fillUniform(ctx, uiState, uniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.kernel = uiState.kernel;
        input.options.precision = uiState.precision;
        input.options.iterations = uiState.iterations;
        input.options.samples = uiState.samples;
        input.options.divergent = uiState.divergent;
        input.options.randomLocality = uiState.randomLocality;
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
        uiStatistics.workgroupCount = statistics.workgroupCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);

        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_DISPATCH] = statistics.cpuRecordDispatchMilliseconds;
        timingValues[TIMING_CPU_RECORD_COMPOSITE] = statistics.cpuRecordCompositeMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("kernel %s | precision %s | iterations %u | samples %u | divergent %d\n",
                        throughputKernelName(uiState.kernel), throughputPrecisionName(uiState.precision),
                        uiState.iterations, uiState.samples, uiState.divergent ? 1 : 0);
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
