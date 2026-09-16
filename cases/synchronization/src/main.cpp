#include "asset_file.h"
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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

// 相位随帧推进时每帧前进多少
static const double PHASE_STEP_PER_FRAME = 1.0 / 200.0;

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS =
    "sync,layers,phase,frozen,draw_commands,command_buffers";

static std::string reportPrefix(const UiState& state, uint32_t drawCommands, uint32_t commandBuffers)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%.0f,%.3f,%d,%u,%u", syncModeName(state.syncMode),
                  static_cast<double>(state.patternLayerCount), static_cast<double>(state.phase),
                  state.advancePhase ? 0 : 1, drawCommands, commandBuffers);
    return std::string(prefix);
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
    uint32_t syncMode = SYNC_MODE_BARRIER;
    float patternLayerCount = 16.0f;
    float frozenPhase = 0.0f;
    bool phaseFrozen = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sync") == 0 && i + 1 < argc) {
            if (!parseSyncMode(argv[i + 1], syncMode)) {
                FATAL("--sync takes %s, %s, %s, %s, %s, %s, %s or %s", syncModeName(0), syncModeName(1),
                      syncModeName(2), syncModeName(3), syncModeName(4), syncModeName(5), syncModeName(6),
                      syncModeName(7));
            }
            ++i;
        } else if (std::strcmp(argv[i], "--layers") == 0 && i + 1 < argc) {
            patternLayerCount = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--phase") == 0 && i + 1 < argc) {
            frozenPhase = static_cast<float>(std::atof(argv[i + 1]));
            phaseFrozen = true;
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

    if (patternLayerCount < 1.0f || patternLayerCount > 256.0f) {
        FATAL("--layers must be between 1 and 256");
    }
    if (frozenPhase < 0.0f || frozenPhase > 1.0f) {
        FATAL("--phase must be between 0 and 1");
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan synchronization Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    SynchronizationRenderer renderer = {};
    createRenderer(ctx, renderer);

    UiState uiState = {};
    uiState.syncMode = syncMode;
    uiState.patternLayerCount = patternLayerCount;
    uiState.phase = frozenPhase;
    uiState.advancePhase = !phaseFrozen;

    UserInterface ui = {};
    uint32_t uiRenderPassFamily = UINT32_MAX;
    ensureUserInterface(ctx, renderer, ui, uiState.syncMode, uiRenderPassFamily);

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
    uint32_t reportCommandBufferCount = 0;

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

        if (verb == "sync") {
            std::string value;
            if (!(stream >> value)) {
                return "err: sync takes a mode name";
            }
            uint32_t parsed = 0;
            if (!parseSyncMode(value.c_str(), parsed)) {
                return "err: unknown sync mode " + value;
            }
            uiState.syncMode = parsed;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "layers") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 256.0) {
                return "err: layers out of range";
            }
            uiState.patternLayerCount = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "phase") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: phase out of range";
            }
            uiState.phase = static_cast<float>(value);
            uiState.advancePhase = false;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "advance") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: advance takes 0 or 1";
            }
            uiState.advancePhase = value == 1;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "begin") {
            usedSegments = true;
            segmentActive = true;
            segmentStartSeconds = nowSeconds();
            reportDrawCallCount = 0;
            reportCommandBufferCount = 0;
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
            const std::string line = reportPrefix(uiState, reportDrawCallCount, reportCommandBufferCount) +
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
            recreateSwapchain(ctx);
            recreateSwapchainTargets(ctx, renderer);
            if (captureRequested && !captureDone) {
                destroyBuffer(ctx, captureBuffer);
                createCaptureBuffer(ctx, captureBuffer);
            }
        }

        // 相位随帧推进时先算好本帧的相位，界面上的滑块显示的就是正在用的值
        if (uiState.advancePhase) {
            uiState.phase = static_cast<float>(std::fmod(static_cast<double>(frameCounter) * PHASE_STEP_PER_FRAME,
                                                         1.0));
        }

        ensureUserInterface(ctx, renderer, ui, uiState.syncMode, uiRenderPassFamily);

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

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        PatternUniform uniform;
        uniform.resolutionAndPhase = glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                                               static_cast<float>(ctx.swapchainExtent.height), uiState.phase,
                                               uiState.patternLayerCount);
        uniform.extra = glm::vec4(0.0f);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.syncMode = uiState.syncMode;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, uniform, statistics);
        if (!frameDrawn) {
            recreateSwapchain(ctx);
            recreateSwapchainTargets(ctx, renderer);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.drawCallCount = statistics.drawCallCount;
        uiStatistics.commandBufferCount = statistics.commandBufferCount;
        uiStatistics.timelineValue = statistics.timelineValue;
        uiStatistics.uniformBufferCoherent = renderer.frames[0].uniformBufferCoherent;
        uiStatistics.uniformFlushPerformed = renderer.frames[0].uniformFlushPerformed;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);
        if (includeInReport) {
            reportDrawCallCount = statistics.drawCallCount;
            reportCommandBufferCount = statistics.commandBufferCount;
        }

        // 按 TimingId 的顺序填满全部计时项，新增计时项时只需要在这里补一行
        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_PRODUCER] = statistics.cpuRecordProducerMilliseconds;
        timingValues[TIMING_CPU_RECORD_SYNC] = statistics.cpuRecordSyncMilliseconds;
        timingValues[TIMING_CPU_RECORD_CONSUMER] = statistics.cpuRecordConsumerMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_CPU_WAIT_SYNC] = statistics.hostWaitMilliseconds;
        timingValues[TIMING_GPU_PRODUCER] = statistics.gpuProducerMilliseconds;
        timingValues[TIMING_GPU_CONSUMER] = statistics.gpuConsumerMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("sync %s | layers %.0f | phase %.3f%s | draw commands %u | command buffers %u | "
                        "timeline %llu\n",
                        syncModeName(uiState.syncMode), static_cast<double>(uiState.patternLayerCount),
                        static_cast<double>(uiState.phase), uiState.advancePhase ? "" : " (frozen)",
                        uiStatistics.drawCallCount, uiStatistics.commandBufferCount,
                        static_cast<unsigned long long>(uiStatistics.timelineValue));
            for (int i = 0; i < TIMING_ID_COUNT; ++i) {
                const TimingWindow& window = timingStore.window[i];
                if (i == TIMING_FRAME) {
                    const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
                    std::printf("  %-30s %7.3f +/- %6.3f ms (%.0f FPS)\n",
                                timingStore.items[i].reportColumn, window.mean, window.standardDeviation,
                                fps);
                } else {
                    std::printf("  %-30s %7.3f +/- %6.3f ms\n", timingStore.items[i].reportColumn,
                                window.mean, window.standardDeviation);
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
        const std::string line = reportPrefix(uiState, reportDrawCallCount, reportCommandBufferCount) +
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
