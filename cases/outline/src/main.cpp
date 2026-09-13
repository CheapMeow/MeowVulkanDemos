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
    "mode,depth_channel,normal_channel,threshold,line_width,extrude,camera,draw_commands";

static bool parseMode(const std::string& value, uint32_t& mode)
{
    if (value == "none") {
        mode = OUTLINE_NONE;
    } else if (value == "post") {
        mode = OUTLINE_POST;
    } else if (value == "hull") {
        mode = OUTLINE_HULL;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix), "%s,%d,%d,%.3f,%.1f,%.3f,%.1f,%u",
                  outlineModeName(state.mode), state.depthChannel ? 1 : 0, state.normalChannel ? 1 : 0,
                  static_cast<double>(state.threshold), static_cast<double>(state.lineWidth),
                  static_cast<double>(state.extrudeDistance), static_cast<double>(state.cameraDistance),
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void rebuildSwapchainResources(VulkanContext& ctx, OutlineRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillSceneUniform(const VulkanContext& ctx, const UiState& state, OutlineSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const float distance = state.cameraDistance;
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0f, 0.18f, distance), glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    outUniform.viewProjection = glm::perspective(glm::radians(38.0f), aspect, 0.1f, 40.0f) * view;
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.modeParams = glm::vec4(static_cast<float>(state.mode), state.depthChannel ? 1.0f : 0.0f,
                                      state.normalChannel ? 1.0f : 0.0f, state.extrudeDistance);
    outUniform.outlineParams = glm::vec4(state.threshold, state.lineWidth, 0.0f, 0.0f);
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
    uint32_t mode = OUTLINE_POST;
    bool depthChannel = true;
    bool normalChannel = true;
    float threshold = 0.005f;
    float lineWidth = 2.0f;
    float extrudeDistance = 0.03f;
    float cameraDistance = 2.6f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (!parseMode(argv[i + 1], mode)) {
                FATAL("--mode takes none, post or hull");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--no-depth-channel") == 0) {
            depthChannel = false;
        } else if (std::strcmp(argv[i], "--no-normal-channel") == 0) {
            normalChannel = false;
        } else if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--line-width") == 0 && i + 1 < argc) {
            lineWidth = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--extrude") == 0 && i + 1 < argc) {
            extrudeDistance = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            cameraDistance = static_cast<float>(std::atof(argv[i + 1]));
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Outline Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    OutlineRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.outputRenderPass, caseTexts, caseTextCount, ui);

    UiState uiState = {};
    uiState.mode = mode;
    uiState.depthChannel = depthChannel;
    uiState.normalChannel = normalChannel;
    uiState.threshold = threshold;
    uiState.lineWidth = lineWidth;
    uiState.extrudeDistance = extrudeDistance;
    uiState.cameraDistance = cameraDistance;

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
            if (!(stream >> value) || !parseMode(value, uiState.mode)) {
                return "err: mode takes none, post or hull";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "depth-channel" || verb == "normal-channel") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: " + verb + " takes 0 or 1";
            }
            if (verb == "depth-channel") {
                uiState.depthChannel = value == 1;
            } else {
                uiState.normalChannel = value == 1;
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "threshold" || verb == "line-width" || verb == "extrude" || verb == "camera") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: " + verb + " takes a positive value";
            }
            if (verb == "threshold") {
                uiState.threshold = static_cast<float>(value);
            } else if (verb == "line-width") {
                uiState.lineWidth = static_cast<float>(value);
            } else if (verb == "extrude") {
                uiState.extrudeDistance = static_cast<float>(value);
            } else {
                uiState.cameraDistance = static_cast<float>(value);
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

        OutlineSceneUniform sceneUniform;
        fillSceneUniform(ctx, uiState, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.options.mode = uiState.mode;
        input.options.depthChannel = uiState.depthChannel;
        input.options.normalChannel = uiState.normalChannel;
        input.options.threshold = uiState.threshold;
        input.options.lineWidth = uiState.lineWidth;
        input.options.extrudeDistance = uiState.extrudeDistance;
        input.options.cameraDistance = uiState.cameraDistance;
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
        timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
        timingValues[TIMING_CPU_RECORD_COMPOSITE] = statistics.cpuRecordCompositeMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("mode %s | depth %d | normal %d | camera %.1f | draw commands %u\n",
                        outlineModeName(uiState.mode), uiState.depthChannel ? 1 : 0,
                        uiState.normalChannel ? 1 : 0, static_cast<double>(uiState.cameraDistance),
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
