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
    "mode,rod_count,rod_width,pan,fxaa_steps,fxaa_threshold,draw_commands";

const char* modeArgumentName(uint32_t mode)
{
    if (mode == ANTIALIAS_MSAA) {
        return "msaa";
    }
    if (mode == ANTIALIAS_FXAA) {
        return "fxaa";
    }
    return "none";
}

struct AppState {
    VulkanContext ctx = {};
    ThinRenderer renderer = {};
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

    uint32_t sampleCount = 4;
    uint64_t frameCounter = 0;
    double previousSeconds = 0.0;
    double startSeconds = 0.0;
    double lastPrintSeconds = 0.0;
    double panOffsetPixels = 0.0;

    ControlServer controlServer;
    bool quitRequested = false;
    bool segmentActive = false;
    double segmentStartSeconds = 0.0;
};

static constexpr double kWarmUpSeconds = 1.5;
static constexpr uint16_t kControlPort = 21000;

static std::string reportPrefix(const UiState& state, const UiStatistics& statistics)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%u,%.3f,%.2f,%u,%.3f,%u", modeArgumentName(state.mode),
                  state.rodCount, state.rodWidthPixels, state.panOffsetPixels, state.fxaaSearchSteps,
                  state.fxaaEdgeThreshold, statistics.drawCallCount);
    return std::string(prefix);
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

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    uint32_t sampleCount = state.sampleCount;
    const VkSampleCountFlags supported =
        state.ctx.physicalDeviceProperties.limits.framebufferColorSampleCounts &
        state.ctx.physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    while (sampleCount > 1) {
        const VkSampleCountFlagBits flag = sampleCount >= 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
        if ((supported & flag) != 0) {
            break;
        }
        sampleCount /= 2;
    }
    state.sampleCount = sampleCount;

    createRenderer(state.ctx, state.renderer, sampleCount);

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
    LOG_I("android thin geometry renderer initialized, samples %u", state.sampleCount);
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

    state.panOffsetPixels += static_cast<double>(state.uiState.panSpeed) * deltaSeconds;
    state.uiState.panOffsetPixels = static_cast<float>(state.panOffsetPixels);

    ThinSceneUniform sceneUniform;
    fillSceneUniform(state.ctx, state.uiState, sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.options.mode = state.uiState.mode;
    input.options.sampleCount = state.uiState.mode == ANTIALIAS_MSAA ? state.sampleCount : 1;
    input.options.rodCount = state.uiState.rodCount;
    input.options.fxaaSearchSteps = state.uiState.fxaaSearchSteps;
    input.options.fxaaEdgeThreshold = state.uiState.fxaaEdgeThreshold;

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
    timingValues[TIMING_CPU_RECORD_GEOMETRY] = statistics.cpuRecordGeometryPassMilliseconds;
    timingValues[TIMING_CPU_RECORD_ANTIALIAS] = statistics.cpuRecordAntialiasMilliseconds;
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
        LOG_I("frame %.1f FPS, mode %s, rods %u", fps, modeArgumentName(state.uiState.mode),
              state.uiState.rodCount);
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
            if (!(stream >> value) || (value != "none" && value != "msaa" && value != "fxaa")) {
                return "err: mode takes none, msaa or fxaa";
            }
            state.uiState.mode = value == "none" ? ANTIALIAS_NONE
                                 : value == "msaa" ? ANTIALIAS_MSAA
                                                   : ANTIALIAS_FXAA;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "rod-count") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 512) {
                return "err: rod-count takes a value between 1 and 512";
            }
            state.uiState.rodCount = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "rod-width") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: rod-width takes a positive pixel count";
            }
            state.uiState.rodWidthPixels = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "pan-speed") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0) {
                return "err: pan-speed takes a non-negative value";
            }
            state.uiState.panSpeed = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "pan") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: pan takes a pixel offset";
            }
            state.panOffsetPixels = value;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "background") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 1.0) {
                return "err: background out of range";
            }
            state.uiState.backgroundIntensity = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "fxaa-steps") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > 16) {
                return "err: fxaa-steps takes a value between 1 and 16";
            }
            state.uiState.fxaaSearchSteps = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "fxaa-threshold") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0 || value > 1.0) {
                return "err: fxaa-threshold out of range";
            }
            state.uiState.fxaaEdgeThreshold = static_cast<float>(value);
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
    state.uiState.mode = ANTIALIAS_MSAA;
    state.uiState.rodCount = 64;
    state.uiState.rodWidthPixels = 1.5f;
    state.uiState.panSpeed = 40.0f;
    state.uiState.panOffsetPixels = 0.0f;
    state.uiState.backgroundIntensity = 0.05f;
    state.uiState.fxaaSearchSteps = 8;
    state.uiState.fxaaEdgeThreshold = 0.125f;

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
