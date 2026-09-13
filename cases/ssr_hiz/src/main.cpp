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

static const float NEAR_PLANE = 0.5f;
static const float FAR_PLANE = 40.0f;
static const float CAMERA_DISTANCE = 12.0f;
static const float CAMERA_PITCH_DEGREES = 2.0f;
static const glm::vec3 CAMERA_TARGET(0.0f, 3.2f, -4.0f);

static const char* const REPORT_HEADER_COLUMNS =
    "reflection,mode,steps,step_length,step_pixels,thickness,max_distance,refine,temporal,jitter,jitter_strength,edge_fade,"
    "average_pyramid,visualization,visualize_level";

static bool parseMarchMode(const std::string& value, uint32_t& mode)
{
    if (value == "view_space") {
        mode = 0;
    } else if (value == "screen_pixel") {
        mode = 1;
    } else if (value == "hiz") {
        mode = 2;
    } else {
        return false;
    }
    return true;
}

static bool parseVisualization(const std::string& value, uint32_t& visualization)
{
    if (value == "off") {
        visualization = 0;
    } else if (value == "pyramids") {
        visualization = 1;
    } else if (value == "step_count") {
        visualization = 2;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state)
{
    char prefix[288];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%u,%.3f,%.1f,%.2f,%.1f,%s,%s,%s,%.3f,%.3f,%s,%s,%u",
                  state.reflection ? "on" : "off", marchModeName(state.marchMode), state.maxSteps, state.stepLength, state.stepPixels,
                  state.thickness, state.maxDistance, state.binaryRefine ? "on" : "off", state.temporal ? "on" : "off",
                  state.jitter ? "on" : "off", state.jitterStrength, state.edgeFade,
                  state.averagePyramid ? "on" : "off", visualizationName(state.visualization),
                  state.visualizeLevel);
    return std::string(prefix);
}

static bool stateChanged(const UiState& a, const UiState& b)
{
    return a.reflection != b.reflection || a.marchMode != b.marchMode || a.maxSteps != b.maxSteps || a.stepLength != b.stepLength ||
           a.stepPixels != b.stepPixels || a.thickness != b.thickness || a.maxDistance != b.maxDistance ||
           a.binaryRefine != b.binaryRefine || a.temporal != b.temporal || a.jitter != b.jitter ||
           a.jitterStrength != b.jitterStrength || a.edgeFade != b.edgeFade ||
           a.visualization != b.visualization || a.visualizeLevel != b.visualizeLevel ||
           a.averagePyramid != b.averagePyramid || a.yawDegrees != b.yawDegrees;
}

static void rebuildSwapchainResources(VulkanContext& ctx, SsrRenderer& renderer, GpuBuffer& captureBuffer,
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

static void fillUniform(const VulkanContext& ctx, const UiState& state, uint64_t frameCounter,
                        bool resetAccumulation, SsrUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, NEAR_PLANE, FAR_PLANE);
    const float yaw = glm::radians(state.yawDegrees);
    const float pitch = glm::radians(CAMERA_PITCH_DEGREES);
    const glm::vec3 offset(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const glm::vec3 eye = CAMERA_TARGET + offset * CAMERA_DISTANCE;
    const glm::mat4 view = glm::lookAt(eye, CAMERA_TARGET, glm::vec3(0.0f, 1.0f, 0.0f));

    outUniform.view = view;
    outUniform.projection = projection;
    outUniform.viewProjection = projection * view;
    outUniform.inverseProjection = glm::inverse(projection);
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height),
                  static_cast<float>(PYRAMID_LEVEL_COUNT), state.reflection ? 1.0f : 0.0f);
    outUniform.marchParams =
        glm::vec4(static_cast<float>(state.maxSteps),
                  state.marchMode == 0 ? state.stepLength : state.stepPixels, state.thickness,
                  state.jitterStrength);
    outUniform.modeParams = glm::vec4(static_cast<float>(state.marchMode), state.binaryRefine ? 1.0f : 0.0f,
                                      state.temporal ? 0.25f : 1.0f, state.jitter ? 1.0f : 0.0f);
    outUniform.miscParams = glm::vec4(static_cast<float>(state.visualization),
                                      static_cast<float>(state.visualizeLevel), state.edgeFade,
                                      state.averagePyramid ? 1.0f : 0.0f);
    outUniform.frameParams = glm::vec4(static_cast<float>(frameCounter & 0xffffu),
                                       resetAccumulation ? 1.0f : 0.0f, NEAR_PLANE, FAR_PLANE);
    outUniform.rangeParams = glm::vec4(state.maxDistance, 0.0f, 0.0f, 0.0f);
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
    uiState.reflection = true;
    uiState.marchMode = 2;
    uiState.maxSteps = 64;
    uiState.stepLength = 0.10f;
    uiState.stepPixels = 4.0f;
    uiState.thickness = 0.35f;
    uiState.maxDistance = 24.0f;
    uiState.binaryRefine = true;
    uiState.temporal = false;
    uiState.jitter = false;
    uiState.jitterStrength = 0.15f;
    uiState.edgeFade = 0.0f;
    uiState.visualization = 0;
    uiState.visualizeLevel = 2;
    uiState.averagePyramid = false;
    uiState.yawDegrees = 0.0f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-reflection") == 0) {
            uiState.reflection = false;
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (!parseMarchMode(argv[i + 1], uiState.marchMode)) {
                FATAL("--mode takes view_space, screen_pixel or hiz");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            uiState.maxSteps = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--step-length") == 0 && i + 1 < argc) {
            uiState.stepLength = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--step-pixels") == 0 && i + 1 < argc) {
            uiState.stepPixels = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--thickness") == 0 && i + 1 < argc) {
            uiState.thickness = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--max-distance") == 0 && i + 1 < argc) {
            uiState.maxDistance = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--refine") == 0) {
            uiState.binaryRefine = true;
        } else if (std::strcmp(argv[i], "--no-refine") == 0) {
            uiState.binaryRefine = false;
        } else if (std::strcmp(argv[i], "--temporal") == 0) {
            uiState.temporal = true;
        } else if (std::strcmp(argv[i], "--jitter") == 0) {
            uiState.jitter = true;
        } else if (std::strcmp(argv[i], "--jitter-strength") == 0 && i + 1 < argc) {
            uiState.jitterStrength = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--edge-fade") == 0 && i + 1 < argc) {
            uiState.edgeFade = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--visualize") == 0 && i + 1 < argc) {
            if (!parseVisualization(argv[i + 1], uiState.visualization)) {
                FATAL("--visualize takes off, pyramids or step_count");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--visualize-level") == 0 && i + 1 < argc) {
            uiState.visualizeLevel = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--average-pyramid") == 0) {
            uiState.averagePyramid = true;
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

    if (uiState.maxSteps < 8 || uiState.maxSteps > 256) {
        FATAL("--steps takes a value between 8 and 256");
    }
    if (uiState.stepLength < 0.005f || uiState.stepLength > 1.0f) {
        FATAL("--step-length takes a value between 0.005 and 1.0");
    }
    if (uiState.stepPixels < 1.0f || uiState.stepPixels > 128.0f) {
        FATAL("--step-pixels takes a value between 1 and 128");
    }
    if (uiState.thickness < 0.01f || uiState.thickness > 4.0f) {
        FATAL("--thickness takes a value between 0.01 and 4.0");
    }
    if (uiState.maxDistance < 2.0f || uiState.maxDistance > 40.0f) {
        FATAL("--max-distance takes a value between 2 and 40");
    }
    if (uiState.visualizeLevel >= PYRAMID_LEVEL_COUNT) {
        FATAL("--visualize-level takes a value between 0 and %d", static_cast<int>(PYRAMID_LEVEL_COUNT) - 1);
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Screen Space Reflection Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    SsrRenderer renderer = {};
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

    UiState previousState = uiState;
    bool stateDirty = true;

    controlServer.onCommand = [&](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "reflection") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: reflection takes on or off";
            }
            uiState.reflection = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || !parseMarchMode(value, uiState.marchMode)) {
                return "err: mode takes view_space, screen_pixel or hiz";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "steps") {
            long value = 0;
            if (!(stream >> value) || value < 8 || value > 256) {
                return "err: steps takes a value between 8 and 256";
            }
            uiState.maxSteps = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "step-length") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.005 || value > 1.0) {
                return "err: step-length takes a value between 0.005 and 1.0";
            }
            uiState.stepLength = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "step-pixels") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 128.0) {
                return "err: step-pixels takes a value between 1 and 128";
            }
            uiState.stepPixels = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "thickness") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.01 || value > 4.0) {
                return "err: thickness takes a value between 0.01 and 4.0";
            }
            uiState.thickness = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "max-distance") {
            double value = 0.0;
            if (!(stream >> value) || value < 2.0 || value > 40.0) {
                return "err: max-distance takes a value between 2 and 40";
            }
            uiState.maxDistance = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "refine") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: refine takes on or off";
            }
            uiState.binaryRefine = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "temporal") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: temporal takes on or off";
            }
            uiState.temporal = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "jitter") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: jitter takes on or off";
            }
            uiState.jitter = value == "on";
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "jitter-strength") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: jitter-strength takes a value between 0.0 and 1.0";
            }
            uiState.jitterStrength = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "edge-fade") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 0.5) {
                return "err: edge-fade takes a value between 0.0 and 0.5";
            }
            uiState.edgeFade = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "visualize") {
            std::string value;
            if (!(stream >> value) || !parseVisualization(value, uiState.visualization)) {
                return "err: visualize takes off, pyramids or step_count";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "visualize-level") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value >= static_cast<long>(PYRAMID_LEVEL_COUNT)) {
                return "err: visualize-level out of range";
            }
            uiState.visualizeLevel = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "average-pyramid") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: average-pyramid takes on or off";
            }
            uiState.averagePyramid = value == "on";
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
            const std::string line = reportPrefix(uiState) + timingReportValueColumns(timingStore);
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
            stateDirty = true;
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

        // 配置变化或交换链重建之后先重置一次时域累积，避免新旧结果混在一起
        if (stateChanged(uiState, previousState)) {
            stateDirty = true;
            previousState = uiState;
        }
        const bool resetAccumulation = stateDirty;
        stateDirty = false;

        SsrUniform uniform;
        fillUniform(ctx, uiState, frameCounter, resetAccumulation, uniform);

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
            stateDirty = true;
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
        timingValues[TIMING_CPU_RECORD_GEOMETRY] = statistics.cpuRecordGeometryMilliseconds;
        timingValues[TIMING_CPU_RECORD_PYRAMID] = statistics.cpuRecordPyramidMilliseconds;
        timingValues[TIMING_CPU_RECORD_REFLECT] = statistics.cpuRecordReflectMilliseconds;
        timingValues[TIMING_CPU_RECORD_PRESENT] = statistics.cpuRecordPresentMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        timingValues[TIMING_GPU_GEOMETRY] = statistics.gpuGeometryMilliseconds;
        timingValues[TIMING_GPU_PYRAMID] = statistics.gpuPyramidMilliseconds;
        timingValues[TIMING_GPU_REFLECT] = statistics.gpuReflectMilliseconds;
        timingValues[TIMING_GPU_PRESENT] = statistics.gpuPresentMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("mode %s | steps %u | length %.3f | pixels %.1f | thickness %.2f | refine %s | "
                        "temporal %s | jitter %s | average %s | visualize %s %u\n",
                        marchModeName(uiState.marchMode), uiState.maxSteps, uiState.stepLength,
                        uiState.stepPixels, uiState.thickness, uiState.binaryRefine ? "on" : "off",
                        uiState.temporal ? "on" : "off", uiState.jitter ? "on" : "off",
                        uiState.averagePyramid ? "on" : "off", visualizationName(uiState.visualization),
                        uiState.visualizeLevel);
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
        const std::string line = reportPrefix(uiState) + timingReportValueColumns(timingStore);
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
