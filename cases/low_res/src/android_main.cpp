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
    "ratio,upsample,depth,bilateral,glow,low_res_width,low_res_height,draw_commands";

bool parseRatio(const std::string& value, uint32_t& ratio)
{
    if (value == "full") {
        ratio = LOWRES_RATIO_FULL;
    } else if (value == "half") {
        ratio = LOWRES_RATIO_HALF;
    } else if (value == "quarter") {
        ratio = LOWRES_RATIO_QUARTER;
    } else {
        return false;
    }
    return true;
}

bool parseUpsample(const std::string& value, uint32_t& upsample)
{
    if (value == "bilinear") {
        upsample = LOWRES_UPSAMPLE_BILINEAR;
    } else if (value == "bilateral") {
        upsample = LOWRES_UPSAMPLE_BILATERAL;
    } else {
        return false;
    }
    return true;
}

bool parseDepthMode(const std::string& value, uint32_t& depthMode)
{
    if (value == "copy") {
        depthMode = LOWRES_DEPTH_COPY;
    } else if (value == "subpass") {
        depthMode = LOWRES_DEPTH_SUBPASS;
    } else if (value == "reconstruct") {
        depthMode = LOWRES_DEPTH_RECONSTRUCT;
    } else {
        return false;
    }
    return true;
}

struct AppState {
    VulkanContext ctx = {};
    LowResRenderer renderer = {};
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
    double sceneTime = 0.0;

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
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%.1f,%.2f,%u,%u,%u",
                  lowResRatioName(state.ratio), lowResUpsampleName(state.upsample),
                  lowResDepthModeName(state.depthMode), static_cast<double>(state.bilateralStrength),
                  static_cast<double>(state.glowIntensity), statistics.lowResWidth, statistics.lowResHeight,
                  statistics.drawCallCount);
    return std::string(prefix);
}

static void fillSceneUniform(const VulkanContext& ctx, const LowResRenderer& renderer, const UiState& state,
                             double sceneTime, LowResSceneUniform& outUniform)
{
    const float aspect = static_cast<float>(ctx.swapchainExtent.width) /
                         static_cast<float>(ctx.swapchainExtent.height);
    outUniform.viewProjection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 30.0f);
    outUniform.viewportParams =
        glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                  static_cast<float>(ctx.swapchainExtent.height), static_cast<float>(renderer.lowResWidth),
                  static_cast<float>(renderer.lowResHeight));
    outUniform.modeParams =
        glm::vec4(static_cast<float>(state.ratio), static_cast<float>(state.upsample),
                  static_cast<float>(state.depthMode), state.bilateralStrength);
    outUniform.miscParams = glm::vec4(static_cast<float>(sceneTime), state.glowIntensity, 0.1f, 30.0f);
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
    LOG_I("android low resolution renderer initialized");
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

    state.sceneTime += static_cast<double>(deltaSeconds);

    LowResSceneUniform sceneUniform;
    fillSceneUniform(state.ctx, state.renderer, state.uiState, state.sceneTime, sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.options.ratio = state.uiState.ratio;
    input.options.upsample = state.uiState.upsample;
    input.options.depthMode = state.uiState.depthMode;
    input.options.bilateralStrength = state.uiState.bilateralStrength;

    FrameStatistics statistics = {};
    const bool frameDrawn =
        drawFrame(state.ctx, state.renderer, state.frameCounter, input, sceneUniform, statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;
    state.uiStatistics.lowResWidth = statistics.lowResWidth;
    state.uiStatistics.lowResHeight = statistics.lowResHeight;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
    timingValues[TIMING_CPU_RECORD_LOWRES] = statistics.cpuRecordLowResMilliseconds;
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
        LOG_I("frame %.1f FPS, ratio %s, upsample %s, low res %ux%u", fps,
              lowResRatioName(state.uiState.ratio), lowResUpsampleName(state.uiState.upsample),
              statistics.lowResWidth, statistics.lowResHeight);
        state.lastPrintSeconds = currentTime;
    }
}

static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "ratio") {
            std::string value;
            if (!(stream >> value) || !parseRatio(value, state.uiState.ratio)) {
                return "err: ratio takes full, half or quarter";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "upsample") {
            std::string value;
            if (!(stream >> value) || !parseUpsample(value, state.uiState.upsample)) {
                return "err: upsample takes bilinear or bilateral";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "depth") {
            std::string value;
            if (!(stream >> value) || !parseDepthMode(value, state.uiState.depthMode)) {
                return "err: depth takes copy, subpass or reconstruct";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "bilateral" || verb == "glow") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 400.0) {
                return "err: " + verb + " out of range";
            }
            if (verb == "bilateral") {
                state.uiState.bilateralStrength = static_cast<float>(value);
            } else {
                state.uiState.glowIntensity = static_cast<float>(value);
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "time") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: time takes seconds";
            }
            state.sceneTime = value;
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
    state.uiState.ratio = LOWRES_RATIO_QUARTER;
    state.uiState.upsample = LOWRES_UPSAMPLE_BILATERAL;
    state.uiState.depthMode = LOWRES_DEPTH_COPY;
    state.uiState.bilateralStrength = 60.0f;
    state.uiState.glowIntensity = 1.2f;

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
