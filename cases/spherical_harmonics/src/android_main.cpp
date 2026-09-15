#ifdef __ANDROID__

#include "asset_file.h"
#include "case_ui.h"
#include "control_server.h"
#include "gpu_clock_lock.h"
#include "renderer.h"
#include "scene.h"
#include "scene_setup.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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
    "bands,background,shading,reference_samples,rotation_deg,exposure,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    ShRenderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
    UiState uiState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};
    std::string reportHeaderColumns;

    EnvironmentParams environment = {};
    float baseCoefficients[SH_MAX_COEFFICIENTS * 3] = {};
    float rotatedCoefficients[SH_MAX_COEFFICIENTS * 3] = {};
    float lastProjectedSky = -1.0f;
    float lastProjectedGlow = -1.0f;
    float lastProjectedExponent = -1.0f;

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
    uint32_t segmentDrawCallCount = 0;
};

static constexpr double kWarmUpSeconds = 1.5;
static constexpr uint16_t kControlPort = 21000;

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%u,%s,%s,%u,%.1f,%.2f,%u", state.bandCount,
                  shBackgroundName(state.background), shShadingName(state.shading),
                  state.referenceSamples, state.rotationDegrees, state.exposure, drawCommands);
    return std::string(prefix);
}

static void packCoefficients(const float* coefficientsRgb, glm::vec4* outPacked)
{
    for (int channel = 0; channel < 3; ++channel) {
        for (int i = 0; i < SH_MAX_COEFFICIENTS; ++i) {
            outPacked[channel * SH_CHANNEL_VEC4_COUNT + i / 4][i % 4] = coefficientsRgb[i * 3 + channel];
        }
    }
}

static void fillUniform(const Camera& camera, float aspectRatio, const UiState& state,
                        const float* coefficientsRgb, ShUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.inverseViewProjection = glm::inverse(matrices.viewProjection);
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.options = glm::vec4(static_cast<float>(state.bandCount),
                                   static_cast<float>(state.background),
                                   static_cast<float>(state.shading),
                                   static_cast<float>(state.referenceSamples));
    outUniform.rotationParams = glm::vec4(glm::radians(state.rotationDegrees), 0.0f, 0.0f, 0.0f);
    outUniform.skyParams = glm::vec4(state.skyIntensity, state.glowIntensity, state.glowExponent, 0.0f);
    outUniform.sunDirection = glm::vec4(defaultEnvironmentParams().sunDirection, 0.0f);
    outUniform.displayParams = glm::vec4(state.exposure, 0.0f, 0.0f, 0.0f);
    packCoefficients(coefficientsRgb, outUniform.sh);
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
    createUserInterface(state.ctx, state.renderer.renderPass, caseTexts, caseTextCount, state.ui);

    // 相机固定在初始位置，安卓端没有键盘输入
    state.camera.position = glm::vec3(0.0f, 0.0f, 3.6f);
    state.camera.yaw = 0.0f;
    state.camera.pitch = 0.0f;
    state.camera.verticalFieldOfView = glm::radians(55.0f);
    state.camera.nearPlane = 0.1f;
    state.camera.farPlane = 100.0f;
    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android spherical harmonics renderer initialized");
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
    LOG_I("swapchain rebuilt to %ux%u (window %dx%d)", state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height, ANativeWindow_getWidth(state.ctx.window),
          ANativeWindow_getHeight(state.ctx.window));
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
        LOG_I("window %dx%d differs from swapchain %ux%u, rebuilding", windowWidth, windowHeight,
              state.ctx.swapchainExtent.width, state.ctx.swapchainExtent.height);
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

    if (state.uiState.skyIntensity != state.lastProjectedSky ||
        state.uiState.glowIntensity != state.lastProjectedGlow ||
        state.uiState.glowExponent != state.lastProjectedExponent) {
        state.environment.skyIntensity = state.uiState.skyIntensity;
        state.environment.glowIntensity = state.uiState.glowIntensity;
        state.environment.glowExponent = state.uiState.glowExponent;
        state.uiState.projectionMilliseconds =
            projectEnvironment(state.environment, state.baseCoefficients) * 1000.0;
        state.lastProjectedSky = state.uiState.skyIntensity;
        state.lastProjectedGlow = state.uiState.glowIntensity;
        state.lastProjectedExponent = state.uiState.glowExponent;
        LOG_I("projected the environment in %.1f ms", state.uiState.projectionMilliseconds);
    }

    std::memcpy(state.rotatedCoefficients, state.baseCoefficients, sizeof(state.rotatedCoefficients));
    rotateShAboutY(state.rotatedCoefficients, SH_MAX_BANDS,
                   glm::radians(state.uiState.rotationDegrees));

    beginUserInterfaceFrame();
    buildUserInterface(state.uiState, state.uiStatistics, state.timingStore, state.gpuClockLockState,
                       state.gpuClockMonitor);
    endUserInterfaceFrame();

    syncSwapchainToWindow(state);

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);

    ShUniform uniform;
    fillUniform(state.camera, aspectRatio, state.uiState, state.rotatedCoefficients, uniform);

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
    timingValues[TIMING_CPU_RECORD_DRAW] = statistics.cpuRecordDrawMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    if (includeInReport) {
        state.segmentDrawCallCount = statistics.drawCallCount;
    }
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, bands %u, background %s, shading %s", fps, state.uiState.bandCount,
              shBackgroundName(state.uiState.background), shShadingName(state.uiState.shading));
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：bands/background/shading/samples/rotation/exposure/sky/sun 改配置，
// begin/end 圈定一段测量并返回一行报告，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "bands") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > SH_MAX_BANDS) {
                return "err: bands takes an integer between 1 and 5";
            }
            state.uiState.bandCount = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "background") {
            std::string value;
            if (!(stream >> value) || (value != "reconstructed" && value != "original")) {
                return "err: background takes reconstructed or original";
            }
            state.uiState.background =
                value == "original" ? SH_BACKGROUND_ORIGINAL : SH_BACKGROUND_RECONSTRUCTED;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "shading") {
            std::string value;
            if (!(stream >> value) || (value != "coefficients" && value != "reference")) {
                return "err: shading takes coefficients or reference";
            }
            state.uiState.shading =
                value == "reference" ? SH_SHADING_REFERENCE : SH_SHADING_COEFFICIENTS;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || value < 16 || value > SH_REFERENCE_MAX_SAMPLES) {
                return "err: samples takes an integer between 16 and 4096";
            }
            state.uiState.referenceSamples = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "rotation" || verb == "exposure" || verb == "sky" || verb == "glow" ||
            verb == "exponent") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "rotation") {
                state.uiState.rotationDegrees = value;
            } else if (verb == "exposure") {
                state.uiState.exposure = value;
            } else if (verb == "sky") {
                state.uiState.skyIntensity = value;
                state.lastProjectedSky = -1.0f;
            } else if (verb == "glow") {
                state.uiState.glowIntensity = value;
                state.lastProjectedGlow = -1.0f;
            } else {
                state.uiState.glowExponent = value;
                state.lastProjectedExponent = -1.0f;
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "begin") {
            state.segmentActive = true;
            state.segmentStartSeconds = nowSeconds();
            state.segmentDrawCallCount = 0;
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
            const std::string row = reportPrefix(state.uiState, state.segmentDrawCallCount) +
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
    state.uiState.cameraMoveSpeed = 3.0f;
    state.uiState.bandCount = 5;
    state.uiState.background = SH_BACKGROUND_ORIGINAL;
    state.uiState.shading = SH_SHADING_COEFFICIENTS;
    state.uiState.referenceSamples = 1024;
    state.uiState.rotationDegrees = 0.0f;
    state.uiState.exposure = 1.0f;
    state.uiState.skyIntensity = 1.0f;
    state.uiState.glowIntensity = 4.0f;
    state.uiState.glowExponent = 4.0f;
    state.environment = defaultEnvironmentParams();

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
