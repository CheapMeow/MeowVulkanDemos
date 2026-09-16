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
    "kind,frequency,octaves,persistence,lacunarity,evaluations,draw_commands";

static bool parseKind(const std::string& value, uint32_t& kind)
{
    if (value == "value") {
        kind = NOISE_VALUE;
    } else if (value == "perlin") {
        kind = NOISE_PERLIN;
    } else if (value == "worley") {
        kind = NOISE_WORLEY;
    } else if (value == "fbm") {
        kind = NOISE_FBM;
    } else if (value == "reference") {
        kind = NOISE_REFERENCE;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, uint32_t evaluations, uint32_t drawCommands)
{
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix), "%s,%.1f,%u,%.2f,%.2f,%u,%u", noiseKindName(state.kind),
                  state.frequency, state.octaves, state.persistence, state.lacunarity, evaluations,
                  drawCommands);
    return std::string(prefix);
}

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建
static void rebuildSwapchainResources(VulkanContext& ctx, NoiseRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillUniform(const UiState& state, NoiseUniform& outUniform)
{
    outUniform.params = glm::vec4(static_cast<float>(state.kind), state.frequency,
                                  static_cast<float>(state.octaves), state.persistence);
    outUniform.misc = glm::vec4(state.lacunarity, state.timeSeconds,
                                static_cast<float>(state.referenceSide), state.displayScale);
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
    uiState.kind = NOISE_FBM;
    uiState.frequency = 12.0f;
    uiState.octaves = 5;
    uiState.persistence = 0.5f;
    uiState.lacunarity = 2.0f;
    uiState.referenceSide = 4;
    uiState.timeSeconds = 0.0f;
    uiState.animate = false;
    uiState.displayScale = 1.0f;

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
        } else if (std::strcmp(argv[i], "--kind") == 0 && i + 1 < argc) {
            if (!parseKind(argv[i + 1], uiState.kind)) {
                FATAL("--kind takes value, perlin, worley, fbm or reference");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--frequency") == 0 && i + 1 < argc) {
            uiState.frequency = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--octaves") == 0 && i + 1 < argc) {
            uiState.octaves = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--persistence") == 0 && i + 1 < argc) {
            uiState.persistence = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--lacunarity") == 0 && i + 1 < argc) {
            uiState.lacunarity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--reference") == 0 && i + 1 < argc) {
            uiState.referenceSide = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            uiState.timeSeconds = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--animate") == 0) {
            uiState.animate = true;
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            uiState.displayScale = static_cast<float>(std::atof(argv[i + 1]));
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Procedural Noise Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    NoiseRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.renderPass, caseTexts, caseTextCount, ui);

    UiStatistics uiStatistics = {};
    uiStatistics.evaluationCount =
        noiseEvaluationCount(uiState.kind, uiState.octaves, uiState.referenceSide);

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

        if (verb == "kind") {
            std::string value;
            if (!(stream >> value) || !parseKind(value, uiState.kind)) {
                return "err: kind takes value, perlin, worley, fbm or reference";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "octaves") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 8) {
                return "err: octaves takes an integer between 1 and 8";
            }
            uiState.octaves = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "reference") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 8) {
                return "err: reference takes an integer between 1 and 8";
            }
            uiState.referenceSide = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "frequency" || verb == "persistence" || verb == "lacunarity" || verb == "time" ||
            verb == "display") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "frequency") {
                uiState.frequency = value;
            } else if (verb == "persistence") {
                uiState.persistence = value;
            } else if (verb == "lacunarity") {
                uiState.lacunarity = value;
            } else if (verb == "time") {
                uiState.timeSeconds = value;
            } else {
                uiState.displayScale = value;
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
            const std::string line =
                reportPrefix(uiState,
                             noiseEvaluationCount(uiState.kind, uiState.octaves, uiState.referenceSide),
                             reportDrawCallCount) +
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

        const double currentTime = nowSeconds();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);
        previousTime = currentTime;
        if (uiState.animate) {
            uiState.timeSeconds += deltaSeconds;
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

        NoiseUniform uniform;
        fillUniform(uiState, uniform);

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
        uiStatistics.renderPassCount = statistics.renderPassCount;
        uiStatistics.evaluationCount =
            noiseEvaluationCount(uiState.kind, uiState.octaves, uiState.referenceSide);

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
        timingValues[TIMING_CPU_RECORD_NOISE] = statistics.cpuRecordNoiseMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("kind %s | frequency %.1f | octaves %u | evaluations %u | draw commands %u\n",
                        noiseKindName(uiState.kind), uiState.frequency, uiState.octaves,
                        uiStatistics.evaluationCount, uiStatistics.drawCallCount);
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
        const std::string line = reportPrefix(uiState, uiStatistics.evaluationCount, reportDrawCallCount) +
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
