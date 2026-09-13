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
const char* const REPORT_HEADER_COLUMNS = "mode,samples,threshold,fragment_count,draw_commands";

// alpha to coverage 需要采样数大于一，单采样下三种方式里只有前两种成立
uint32_t effectiveMode(uint32_t mode, uint32_t sampleCount)
{
    if (mode == ALPHA_MODE_TO_COVERAGE && sampleCount == 1) {
        return ALPHA_MODE_TEST;
    }
    return mode;
}

struct AppState {
    VulkanContext ctx = {};
    AlphaRenderer renderer = {};
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
    double accumulatedAngle = 0.0;

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
    std::snprintf(prefix, sizeof(prefix), "%s,%u,%.3f,%u,%u",
                  alphaModeName(effectiveMode(state.mode, state.sampleCount)), state.sampleCount,
                  state.threshold, statistics.fragmentCount, statistics.drawCallCount);
    return std::string(prefix);
}

static void fillSceneUniform(const VulkanContext& ctx, const UiState& state, float accumulatedAngle,
                             AlphaSceneUniform& outUniform)
{
    outUniform.viewportParams = glm::vec4(static_cast<float>(ctx.swapchainExtent.width),
                                          static_cast<float>(ctx.swapchainExtent.height), 0.0f, 0.0f);
    outUniform.colorParams = glm::vec4(state.backgroundIntensity, state.backgroundIntensity,
                                       state.backgroundIntensity, 0.0f);
    outUniform.modeParams = glm::vec4(static_cast<float>(effectiveMode(state.mode, state.sampleCount)),
                                      state.threshold, 0.0f, 0.0f);
    outUniform.animationParams = glm::vec4(accumulatedAngle, 0.0f, 0.0f, 0.0f);
}

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    const VkSampleCountFlags supported =
        state.ctx.physicalDeviceProperties.limits.framebufferColorSampleCounts &
        state.ctx.physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    uint32_t sampleCount = state.uiState.sampleCount;
    while (sampleCount > 1) {
        const VkSampleCountFlagBits flag = sampleCount >= 8   ? VK_SAMPLE_COUNT_8_BIT
                                           : sampleCount >= 4 ? VK_SAMPLE_COUNT_4_BIT
                                           : sampleCount >= 2 ? VK_SAMPLE_COUNT_2_BIT
                                                              : VK_SAMPLE_COUNT_1_BIT;
        if ((supported & flag) != 0) {
            break;
        }
        sampleCount /= 2;
    }
    state.uiState.sampleCount = sampleCount;

    createRenderer(state.ctx, state.renderer, sampleCount);

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.tonemapRenderPass, caseTexts, caseTextCount, state.ui);

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android alpha modes renderer initialized, samples %u", state.uiState.sampleCount);
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

    state.accumulatedAngle += static_cast<double>(state.uiState.rotationSpeed) * deltaSeconds;

    AlphaSceneUniform sceneUniform;
    fillSceneUniform(state.ctx, state.uiState, static_cast<float>(state.accumulatedAngle), sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.options.mode = effectiveMode(state.uiState.mode, state.uiState.sampleCount);
    input.options.sampleCount = state.uiState.sampleCount;
    input.options.threshold = state.uiState.threshold;
    input.options.accumulatedAngle = static_cast<float>(state.accumulatedAngle);

    FrameStatistics statistics = {};
    const bool frameDrawn =
        drawFrame(state.ctx, state.renderer, state.frameCounter, input, sceneUniform, statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;
    state.uiStatistics.fragmentCount = statistics.fragmentCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_GEOMETRY] = statistics.cpuRecordGeometryPassMilliseconds;
    timingValues[TIMING_CPU_RECORD_RESOLVE] = statistics.cpuRecordResolveMilliseconds;
    timingValues[TIMING_CPU_RECORD_TONEMAP] = statistics.cpuRecordTonemapMilliseconds;
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
        LOG_I("frame %.1f FPS, mode %s, samples %u, fragment calls %u", fps,
              alphaModeName(effectiveMode(state.uiState.mode, state.uiState.sampleCount)),
              state.uiState.sampleCount, statistics.fragmentCount);
        state.lastPrintSeconds = currentTime;
    }
}

static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || (value != 1 && value != 2 && value != 4 && value != 8)) {
                return "err: samples takes 1, 2, 4 or 8";
            }
            state.uiState.sampleCount = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || (value != "test" && value != "blend" && value != "coverage")) {
                return "err: mode takes test, blend or coverage";
            }
            state.uiState.mode = value == "test" ? ALPHA_MODE_TEST
                                 : value == "blend" ? ALPHA_MODE_BLEND
                                                    : ALPHA_MODE_TO_COVERAGE;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "threshold" || verb == "background" || verb == "rotation-speed") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0 || value > 2.0) {
                return "err: " + verb + " out of range";
            }
            if (verb == "threshold") {
                state.uiState.threshold = static_cast<float>(value);
            } else if (verb == "background") {
                state.uiState.backgroundIntensity = static_cast<float>(value);
            } else {
                state.uiState.rotationSpeed = static_cast<float>(value);
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "angle") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: angle takes a value in radians";
            }
            state.accumulatedAngle = value;
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
    state.uiState.mode = ALPHA_MODE_TO_COVERAGE;
    state.uiState.sampleCount = 4;
    state.uiState.threshold = 0.5f;
    state.uiState.backgroundIntensity = 0.06f;
    state.uiState.rotationSpeed = 0.6f;

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
