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

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS =
    "mode,depth_channel,normal_channel,threshold,line_width,extrude,camera,draw_commands";

bool parseMode(const std::string& value, uint32_t& mode)
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

struct AppState {
    VulkanContext ctx = {};
    OutlineRenderer renderer = {};
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
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix), "%s,%d,%d,%.3f,%.1f,%.3f,%.1f,%u",
                  outlineModeName(state.mode), state.depthChannel ? 1 : 0, state.normalChannel ? 1 : 0,
                  static_cast<double>(state.threshold), static_cast<double>(state.lineWidth),
                  static_cast<double>(state.extrudeDistance), static_cast<double>(state.cameraDistance),
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void fillSceneUniform(const VulkanContext& ctx, const UiState& state, OutlineSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.18f, state.cameraDistance),
                                       glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    outUniform.viewProjection = glm::perspective(glm::radians(38.0f), aspect, 0.1f, 40.0f) * view;
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.modeParams = glm::vec4(static_cast<float>(state.mode), state.depthChannel ? 1.0f : 0.0f,
                                      state.normalChannel ? 1.0f : 0.0f, state.extrudeDistance);
    outUniform.outlineParams = glm::vec4(state.threshold, state.lineWidth, 0.0f, 0.0f);
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
    LOG_I("android outline renderer initialized");
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
    LOG_I("swapchain rebuilt to %ux%u", state.ctx.swapchainExtent.width, state.ctx.swapchainExtent.height);
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

    OutlineSceneUniform sceneUniform;
    fillSceneUniform(state.ctx, state.uiState, sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.options.mode = state.uiState.mode;
    input.options.depthChannel = state.uiState.depthChannel;
    input.options.normalChannel = state.uiState.normalChannel;
    input.options.threshold = state.uiState.threshold;
    input.options.lineWidth = state.uiState.lineWidth;
    input.options.extrudeDistance = state.uiState.extrudeDistance;
    input.options.cameraDistance = state.uiState.cameraDistance;

    FrameStatistics statistics = {};
    const bool frameDrawn =
        drawFrame(state.ctx, state.renderer, state.frameCounter, input, sceneUniform, statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
    timingValues[TIMING_CPU_RECORD_COMPOSITE] = statistics.cpuRecordCompositeMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, mode %s, draw commands %u", fps, outlineModeName(state.uiState.mode),
              statistics.drawCallCount);
        state.lastPrintSeconds = currentTime;
    }
}

static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || !parseMode(value, state.uiState.mode)) {
                return "err: mode takes none, post or hull";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "depth-channel" || verb == "normal-channel") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: " + verb + " takes 0 or 1";
            }
            if (verb == "depth-channel") {
                state.uiState.depthChannel = value == 1;
            } else {
                state.uiState.normalChannel = value == 1;
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "threshold" || verb == "line-width" || verb == "extrude" || verb == "camera") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: " + verb + " takes a positive value";
            }
            if (verb == "threshold") {
                state.uiState.threshold = static_cast<float>(value);
            } else if (verb == "line-width") {
                state.uiState.lineWidth = static_cast<float>(value);
            } else if (verb == "extrude") {
                state.uiState.extrudeDistance = static_cast<float>(value);
            } else {
                state.uiState.cameraDistance = static_cast<float>(value);
            }
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
    state.uiState.mode = OUTLINE_POST;
    state.uiState.depthChannel = true;
    state.uiState.normalChannel = true;
    state.uiState.threshold = 0.005f;
    state.uiState.lineWidth = 2.0f;
    state.uiState.extrudeDistance = 0.03f;
    state.uiState.cameraDistance = 2.6f;

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
