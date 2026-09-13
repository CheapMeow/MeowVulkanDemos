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

static const float kNearPlane = 1.0f;
static const float kFarPlane = 25.0f;
static const float kCameraDistance = 4.2f;
static const glm::vec3 kCameraTarget(0.0f, 0.0f, -2.2f);

const char* const REPORT_HEADER_COLUMNS =
    "culling,extreme,level_mode,level,depth_source,expansion,visible,culled,draw_commands";

bool parseExtreme(const std::string& value, uint32_t& extreme)
{
    if (value == "farthest") {
        extreme = 0;
    } else if (value == "nearest") {
        extreme = 1;
    } else {
        return false;
    }
    return true;
}

bool parseLevelMode(const std::string& value, uint32_t& levelMode)
{
    if (value == "fixed") {
        levelMode = 0;
    } else if (value == "auto") {
        levelMode = 1;
    } else {
        return false;
    }
    return true;
}

bool parseDepthSource(const std::string& value, bool& currentFrameDepth)
{
    if (value == "previous") {
        currentFrameDepth = false;
    } else if (value == "current") {
        currentFrameDepth = true;
    } else {
        return false;
    }
    return true;
}

struct AppState {
    VulkanContext ctx = {};
    HzbRenderer renderer = {};
    UserInterface ui = {};
    UiState uiState = {};
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

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[288];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%u,%s,%.2f,%u,%u,%u",
                  state.occlusionCulling ? "on" : "off", hzbExtremeName(state.pyramidExtreme),
                  hzbLevelModeName(state.levelMode), state.level,
                  hzbDepthSourceName(state.currentFrameDepth), state.expansion,
                  statistics.visibleInstanceCount, statistics.culledInstanceCount,
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void fillUniform(const VulkanContext& ctx, const UiState& state, uint32_t instanceCount,
                        HzbSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const float yaw = glm::radians(state.yawDegrees);
    const glm::vec3 eye(std::sin(yaw) * kCameraDistance, 0.35f, std::cos(yaw) * kCameraDistance);
    outUniform.viewProjection = glm::perspective(glm::radians(60.0f), aspect, kNearPlane, kFarPlane) *
                                glm::lookAt(eye, kCameraTarget, glm::vec3(0.0f, 1.0f, 0.0f));
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), state.expansion,
                  state.levelMode == 0 ? 0.0f : 1.0f);
    outUniform.modeParams = glm::vec4(state.occlusionCulling ? 1.0f : 0.0f,
                                      static_cast<float>(state.pyramidExtreme),
                                      static_cast<float>(state.level), kNearPlane);
    outUniform.miscParams = glm::vec4(static_cast<float>(instanceCount), state.visualize ? 1.0f : 0.0f,
                                      kFarPlane, state.currentFrameDepth ? 1.0f : 0.0f);
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
    LOG_I("android hzb renderer initialized");
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
}

static void rebuildSwapchain(AppState& state)
{
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    destroySwapchain(state.ctx);
    createSwapchain(state.ctx);
    recreateSwapchainTargets(state.ctx, state.renderer);
    state.swapchainReady = true;
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

    HzbSceneUniform uniform;
    fillUniform(state.ctx, state.uiState, state.renderer.instanceCount, uniform);

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
    state.uiStatistics.visibleInstanceCount = statistics.visibleInstanceCount;
    state.uiStatistics.culledInstanceCount = statistics.culledInstanceCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_PREPASS] = statistics.cpuRecordPrepassMilliseconds;
    timingValues[TIMING_CPU_RECORD_CULL] = statistics.cpuRecordCullMilliseconds;
    timingValues[TIMING_CPU_RECORD_PYRAMID] = statistics.cpuRecordPyramidMilliseconds;
    timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
    timingValues[TIMING_CPU_RECORD_PRESENT] = statistics.cpuRecordPresentMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    timingValues[TIMING_GPU_PREPASS] = statistics.gpuPrepassMilliseconds;
    timingValues[TIMING_GPU_CULL] = statistics.gpuCullMilliseconds;
    timingValues[TIMING_GPU_PYRAMID] = statistics.gpuPyramidMilliseconds;
    timingValues[TIMING_GPU_SCENE] = statistics.gpuSceneMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, culling %s, extreme %s, visible %u, culled %u", fps,
              state.uiState.occlusionCulling ? "on" : "off",
              hzbExtremeName(state.uiState.pyramidExtreme), state.uiStatistics.visibleInstanceCount,
              state.uiStatistics.culledInstanceCount);
        state.lastPrintSeconds = currentTime;
    }
}

static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "culling") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: culling takes on or off";
            }
            state.uiState.occlusionCulling = value == "on";
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "extreme") {
            std::string value;
            if (!(stream >> value) || !parseExtreme(value, state.uiState.pyramidExtreme)) {
                return "err: extreme takes farthest or nearest";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "level-mode") {
            std::string value;
            if (!(stream >> value) || !parseLevelMode(value, state.uiState.levelMode)) {
                return "err: level-mode takes fixed or auto";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "level") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value >= static_cast<long>(HZB_LEVEL_COUNT)) {
                return "err: level out of range";
            }
            state.uiState.level = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "depth-source") {
            std::string value;
            if (!(stream >> value) || !parseDepthSource(value, state.uiState.currentFrameDepth)) {
                return "err: depth-source takes previous or current";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "expansion") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 2.0) {
                return "err: expansion takes a value between 1.0 and 2.0";
            }
            state.uiState.expansion = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "visualize") {
            std::string value;
            if (!(stream >> value) || (value != "on" && value != "off")) {
                return "err: visualize takes on or off";
            }
            state.uiState.visualize = value == "on";
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
            const std::string row = reportPrefix(state.uiState, state.uiStatistics) +
                                    timingReportValueColumns(state.timingStore);
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
    state.uiState.occlusionCulling = true;
    state.uiState.pyramidExtreme = 0;
    state.uiState.levelMode = 0;
    state.uiState.level = 0;
    state.uiState.currentFrameDepth = false;
    state.uiState.expansion = 1.0f;
    state.uiState.visualize = false;
    state.uiState.yawDegrees = 0.0f;

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
