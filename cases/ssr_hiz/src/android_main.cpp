#ifdef __ANDROID__

#include "asset_file.h"
#include "case_ui.h"
#include "control_server.h"
#include "gpu_clock_lock.h"
#include "renderer.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <imgui_impl_android.h>

#define ANDROID_LOG_TAG "MeowVulkanDemo"
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)

namespace {

static const float kNearPlane = 0.5f;
static const float kFarPlane = 40.0f;
static const float kCameraDistance = 12.0f;
static const float kCameraPitchDegrees = 2.0f;
static const glm::vec3 kCameraTarget(0.0f, 3.2f, -4.0f);

const char* const REPORT_HEADER_COLUMNS =
    "reflection,mode,steps,step_length,step_pixels,thickness,max_distance,refine,temporal,jitter,jitter_strength,edge_fade,"
    "average_pyramid,visualization,visualize_level";

bool parseMarchMode(const std::string& value, uint32_t& mode)
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

bool parseVisualization(const std::string& value, uint32_t& visualization)
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

struct AppState {
    VulkanContext ctx = {};
    SsrRenderer renderer = {};
    UserInterface ui = {};
    UiState uiState = {};
    UiState previousState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};
    std::string reportHeaderColumns;

    bool vulkanInitialized = false;
    bool swapchainReady = false;
    bool windowValid = false;
    bool animating = false;
    bool destroying = false;
    bool started = false;
    bool stateDirty = true;

    uint64_t frameCounter = 0;
    double previousSeconds = 0.0;
    double startSeconds = 0.0;
    double lastPrintSeconds = 0.0;

    ControlServer controlServer;
    bool quitRequested = false;
    bool segmentActive = false;
    double segmentStartSeconds = 0.0;
};

static constexpr double kWarmUpSeconds = 1.5;
static constexpr uint16_t kControlPort = 21000;

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

static void fillUniform(const VulkanContext& ctx, const UiState& state, uint64_t frameCounter,
                        bool resetAccumulation, SsrUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, kNearPlane, kFarPlane);
    const float yaw = glm::radians(state.yawDegrees);
    const float pitch = glm::radians(kCameraPitchDegrees);
    const glm::vec3 offset(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                           std::cos(yaw) * std::cos(pitch));
    const glm::vec3 eye = kCameraTarget + offset * kCameraDistance;
    const glm::mat4 view = glm::lookAt(eye, kCameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));

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
                                       resetAccumulation ? 1.0f : 0.0f, kNearPlane, kFarPlane);
    outUniform.rangeParams = glm::vec4(state.maxDistance, 0.0f, 0.0f, 0.0f);
}

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    createRenderer(state.ctx, state.renderer);
    state.uiStatistics.instanceCount = state.renderer.instanceCount;

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.outputRenderPass, caseTexts, caseTextCount, state.ui);

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android screen space reflection renderer initialized");
}

static void recreateSurfaceAndSwapchain(AppState& state)
{
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    destroySwapchain(state.ctx);
    destroyWindowSurface(state.ctx);
    createWindowSurface(state.ctx, state.ctx.window);
    createSwapchain(state.ctx);
    recreateSwapchainTargets(state.ctx, state.renderer);
    state.swapchainReady = true;
    state.stateDirty = true;
}

static void rebuildSwapchain(AppState& state)
{
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    destroySwapchain(state.ctx);
    createSwapchain(state.ctx);
    recreateSwapchainTargets(state.ctx, state.renderer);
    state.swapchainReady = true;
    state.stateDirty = true;
}

static void syncSwapchainToWindow(AppState& state)
{
    const int windowWidth = ANativeWindow_getWidth(state.ctx.window);
    const int windowHeight = ANativeWindow_getHeight(state.ctx.window);
    if (windowWidth == 0 || windowHeight == 0) {
        return;
    }
    if (static_cast<uint32_t>(windowWidth) != state.ctx.swapchainExtent.width ||
        static_cast<uint32_t>(windowHeight) != state.ctx.swapchainExtent.height) {
        rebuildSwapchain(state);
    }
}

static void teardownSwapchain(AppState& state)
{
    if (!state.swapchainReady) {
        return;
    }
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    destroySwapchain(state.ctx);
    destroyWindowSurface(state.ctx);
    state.swapchainReady = false;
}

static void handleAppCommand(android_app* app, int32_t command)
{
    AppState& state = *static_cast<AppState*>(app->userData);
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (!state.vulkanInitialized) {
                initializeRendererStack(state, app);
            } else {
                state.ctx.window = app->window;
                recreateSurfaceAndSwapchain(state);
            }
            state.windowValid = true;
            state.animating = true;
            break;
        case APP_CMD_TERM_WINDOW:
            state.windowValid = false;
            state.animating = false;
            teardownSwapchain(state);
            break;
        case APP_CMD_GAINED_FOCUS:
            state.animating = state.windowValid;
            break;
        case APP_CMD_LOST_FOCUS:
            state.animating = false;
            break;
        case APP_CMD_DESTROY:
            state.destroying = true;
            break;
        default:
            break;
    }
}

static int32_t handleInputEvent(android_app* /*app*/, AInputEvent* event)
{
    ImGui_ImplAndroid_HandleInputEvent(event);
    return 1;
}

static void drawOneFrame(AppState& state)
{
    const double currentTime = nowSeconds();
    const float deltaSeconds = static_cast<float>(currentTime - state.previousSeconds);
    state.previousSeconds = currentTime;
    if (!state.started) {
        state.started = true;
        return;
    }

    beginUserInterfaceFrame();
    buildUserInterface(state.uiState, state.uiStatistics, state.timingStore, state.gpuClockLockState,
                       state.gpuClockMonitor);
    endUserInterfaceFrame();
    syncSwapchainToWindow(state);

    if (stateChanged(state.uiState, state.previousState)) {
        state.stateDirty = true;
        state.previousState = state.uiState;
    }
    const bool resetAccumulation = state.stateDirty;
    state.stateDirty = false;

    SsrUniform uniform;
    fillUniform(state.ctx, state.uiState, state.frameCounter, resetAccumulation, uniform);

    FrameInput input = {};
    input.drawUserInterface = true;

    FrameStatistics statistics = {};
    const bool frameDrawn =
        drawFrame(state.ctx, state.renderer, state.frameCounter, input, uniform, statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;

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
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, mode %s, steps %u, reflect %.3f ms", fps,
              marchModeName(state.uiState.marchMode), state.uiState.maxSteps,
              statistics.gpuReflectMilliseconds);
        state.lastPrintSeconds = currentTime;
    }
}

static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "reflection") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: reflection takes on or off";
            }
            state.uiState.reflection = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || !parseMarchMode(value, state.uiState.marchMode)) {
                return "err: mode takes view_space, screen_pixel or hiz";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "steps") {
            long value = 0;
            if (!(stream >> value) || value < 8 || value > 256) {
                return "err: steps takes a value between 8 and 256";
            }
            state.uiState.maxSteps = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "step-length") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.005 || value > 1.0) {
                return "err: step-length takes a value between 0.005 and 1.0";
            }
            state.uiState.stepLength = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "step-pixels") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 128.0) {
                return "err: step-pixels takes a value between 1 and 128";
            }
            state.uiState.stepPixels = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "thickness") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.01 || value > 4.0) {
                return "err: thickness takes a value between 0.01 and 4.0";
            }
            state.uiState.thickness = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "max-distance") {
            double value = 0.0;
            if (!(stream >> value) || value < 2.0 || value > 40.0) {
                return "err: max-distance takes a value between 2 and 40";
            }
            state.uiState.maxDistance = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "refine") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: refine takes on or off";
            }
            state.uiState.binaryRefine = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "temporal") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: temporal takes on or off";
            }
            state.uiState.temporal = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "jitter") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: jitter takes on or off";
            }
            state.uiState.jitter = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "jitter-strength") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: jitter-strength takes a value between 0.0 and 1.0";
            }
            state.uiState.jitterStrength = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "edge-fade") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 0.5) {
                return "err: edge-fade takes a value between 0.0 and 0.5";
            }
            state.uiState.edgeFade = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "visualize") {
            std::string value;
            if (!(stream >> value) || !parseVisualization(value, state.uiState.visualization)) {
                return "err: visualize takes off, pyramids or step_count";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "visualize-level") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value >= static_cast<long>(PYRAMID_LEVEL_COUNT)) {
                return "err: visualize-level out of range";
            }
            state.uiState.visualizeLevel = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "average-pyramid") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: average-pyramid takes on or off";
            }
            state.uiState.averagePyramid = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "yaw") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: yaw takes degrees";
            }
            state.uiState.yawDegrees = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "begin") {
            state.segmentActive = true;
            state.segmentStartSeconds = nowSeconds();
            resetTimingReport(state.timingStore);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "end") {
            if (!state.segmentActive) {
                return "err: no segment started";
            }
            if (state.timingStore.report[TIMING_FRAME].sampleCount == 0) {
                return "err: segment had no sampled frames";
            }
            const std::string row =
                reportPrefix(state.uiState) + timingReportValueColumns(state.timingStore);
            state.segmentActive = false;
            resetTimingReport(state.timingStore);
            return "row " + row;
        }
        if (verb == "quit") {
            state.quitRequested = true;
            return "ok";
        }
        return "err: unknown command";
    };
}

}  // namespace

void android_main(android_app* app)
{
    AppState state;
    state.uiState.reflection = true;
    state.uiState.marchMode = 2;
    state.uiState.maxSteps = 64;
    state.uiState.stepLength = 0.10f;
    state.uiState.stepPixels = 4.0f;
    state.uiState.thickness = 0.35f;
    state.uiState.maxDistance = 24.0f;
    state.uiState.binaryRefine = true;
    state.uiState.temporal = false;
    state.uiState.jitter = false;
    state.uiState.jitterStrength = 0.15f;
    state.uiState.edgeFade = 0.0f;
    state.uiState.visualization = 0;
    state.uiState.visualizeLevel = 2;
    state.uiState.averagePyramid = false;
    state.uiState.yawDegrees = 0.0f;
    state.previousState = state.uiState;

    app->userData = &state;
    app->onAppCmd = handleAppCommand;
    app->onInputEvent = handleInputEvent;

    setAndroidAssetManager(app->activity->assetManager);
    detectGpuClockLockState(state.gpuClockLockState);
    createVulkanContext(state.ctx, false);

    installControlHandler(state);
    state.controlServer.start(kControlPort);

    const double frameBudgetSeconds = 1.0 / 60.0;

    while (!state.destroying) {
        state.controlServer.pump();
        if (state.quitRequested) {
            break;
        }

        while (true) {
            const int pollTimeout = (state.animating && state.swapchainReady) ? 0 : -1;
            int events = 0;
            android_poll_source* source = nullptr;
            const int result = ALooper_pollOnce(pollTimeout, nullptr, &events,
                                                 reinterpret_cast<void**>(&source));
            if (result < 0) {
                break;
            }
            if (source != nullptr) {
                source->process(app, source);
            }
            if (state.destroying) {
                break;
            }
        }

        if (state.animating && state.swapchainReady) {
            const double frameStart = nowSeconds();
            drawOneFrame(state);
            const double elapsed = nowSeconds() - frameStart;
            if (elapsed < frameBudgetSeconds) {
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(frameBudgetSeconds - elapsed)));
            }
        }
    }

    state.controlServer.stop();
    teardownSwapchain(state);
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    if (state.vulkanInitialized) {
        destroyUserInterface(state.ctx, state.ui);
        destroyRenderer(state.ctx, state.renderer);
    }
    destroyVulkanContext(state.ctx);

    if (state.quitRequested) {
        _exit(0);
    }
}

#endif
