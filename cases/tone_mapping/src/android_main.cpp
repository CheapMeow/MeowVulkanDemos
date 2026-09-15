#ifdef __ANDROID__

#include "asset_file.h"
#include "case_ui.h"
#include "control_server.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
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

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS =
    "operator,channel,encoding,exposure_ev,white_point,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    ToneMapRenderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
    UiState uiState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};
    std::string reportHeaderColumns;

    MeshData objectMesh = {};
    MeshData barMesh = {};

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

static bool parseOperator(const std::string& value, uint32_t& op)
{
    if (value == "none") {
        op = TONE_MAP_OP_NONE;
    } else if (value == "reinhard") {
        op = TONE_MAP_OP_REINHARD;
    } else if (value == "reinhard_extended") {
        op = TONE_MAP_OP_REINHARD_EXTENDED;
    } else if (value == "aces") {
        op = TONE_MAP_OP_ACES;
    } else if (value == "filmic") {
        op = TONE_MAP_OP_FILMIC;
    } else {
        return false;
    }
    return true;
}

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%.2f,%.2f,%u", toneMapOperatorName(state.toneMapOperator),
                  toneMapChannelName(state.toneMapChannel), toneMapEncodingName(state.outputEncoding),
                  state.exposureEv, state.whitePoint, drawCommands);
    return std::string(prefix);
}

static void fillUniform(const Camera& camera, float aspectRatio, const UiState& state,
                        ToneMapUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.inverseViewProjection = glm::inverse(matrices.viewProjection);
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.lightDirection = glm::vec4(glm::normalize(glm::vec3(0.45f, 0.75f, 0.35f)),
                                          state.lightIntensity);
    outUniform.sunDirection = glm::vec4(glm::normalize(glm::vec3(0.35f, 0.30f, -0.88f)),
                                        state.sunIntensity);
    outUniform.skyParams = glm::vec4(state.skyIntensity, 1.6f, 0.0f, 0.0f);
    outUniform.operatorParams = glm::vec4(static_cast<float>(state.toneMapOperator),
                                          static_cast<float>(state.toneMapChannel),
                                          std::exp2(state.exposureEv), state.whitePoint);
    outUniform.encodingParams = glm::vec4(static_cast<float>(state.outputEncoding), 0.0f, 0.0f, 0.0f);
}

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    loadObjFromMemory(readAssetBytes("backpack/backpack.obj"), state.objectMesh);
    buildReferenceBarMesh(referenceBarValues(), state.barMesh);

    createRenderer(state.ctx, state.renderer, state.objectMesh, state.barMesh);

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.outputRenderPass, caseTexts, caseTextCount, state.ui);

    // 相机固定在初始位置，安卓端没有键盘输入
    state.camera.position = glm::vec3(0.0f, 0.9f, 3.2f);
    state.camera.yaw = 0.0f;
    state.camera.pitch = -0.07f;
    state.camera.verticalFieldOfView = glm::radians(60.0f);
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
    LOG_I("android tone mapping renderer initialized");
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

    beginUserInterfaceFrame();
    buildUserInterface(state.uiState, state.uiStatistics, state.timingStore, state.gpuClockLockState,
                       state.gpuClockMonitor);
    endUserInterfaceFrame();

    syncSwapchainToWindow(state);

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);

    ToneMapUniform uniform;
    fillUniform(state.camera, aspectRatio, state.uiState, uniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.options.op = state.uiState.toneMapOperator;
    input.options.channel = state.uiState.toneMapChannel;
    input.options.encoding = state.uiState.outputEncoding;
    input.options.exposureEv = state.uiState.exposureEv;
    input.options.whitePoint = state.uiState.whitePoint;

    FrameStatistics statistics = {};
    const bool frameDrawn =
        drawFrame(state.ctx, state.renderer, state.frameCounter, input, uniform, statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;
    state.uiStatistics.renderPassCount = statistics.renderPassCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_SCENE] = statistics.cpuRecordSceneMilliseconds;
    timingValues[TIMING_CPU_RECORD_TONEMAP] = statistics.cpuRecordTonemapMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    timingValues[TIMING_GPU_SCENE] = statistics.gpuSceneMilliseconds;
    timingValues[TIMING_GPU_TONEMAP] = statistics.gpuTonemapMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    if (includeInReport) {
        state.segmentDrawCallCount = statistics.drawCallCount;
    }
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, op %s, encoding %s, EV %.2f", fps,
              toneMapOperatorName(state.uiState.toneMapOperator),
              toneMapEncodingName(state.uiState.outputEncoding), state.uiState.exposureEv);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：op/channel/encoding/exposure/white-point/light/sky/sun 改配置，
// begin/end 圈定一段测量并返回一行报告，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "op") {
            std::string value;
            if (!(stream >> value) || !parseOperator(value, state.uiState.toneMapOperator)) {
                return "err: op takes none, reinhard, reinhard_extended, aces or filmic";
            }
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "channel") {
            std::string value;
            if (!(stream >> value) || (value != "per_channel" && value != "luminance")) {
                return "err: channel takes per_channel or luminance";
            }
            state.uiState.toneMapChannel =
                value == "luminance" ? TONE_MAP_CHANNEL_LUMINANCE : TONE_MAP_CHANNEL_PER_CHANNEL;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "encoding") {
            std::string value;
            if (!(stream >> value) || (value != "linear" && value != "gamma" && value != "srgb")) {
                return "err: encoding takes linear, gamma or srgb";
            }
            state.uiState.outputEncoding = value == "gamma"  ? TONE_MAP_ENCODING_GAMMA
                                           : value == "srgb" ? TONE_MAP_ENCODING_SRGB
                                                             : TONE_MAP_ENCODING_LINEAR;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "exposure") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: exposure takes a number";
            }
            state.uiState.exposureEv = value;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "white-point") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: white-point takes a number";
            }
            state.uiState.whitePoint = value;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "light" || verb == "sky" || verb == "sun") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "light") {
                state.uiState.lightIntensity = value;
            } else if (verb == "sky") {
                state.uiState.skyIntensity = value;
            } else {
                state.uiState.sunIntensity = value;
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
    state.uiState.toneMapOperator = TONE_MAP_OP_ACES;
    state.uiState.toneMapChannel = TONE_MAP_CHANNEL_PER_CHANNEL;
    state.uiState.outputEncoding = TONE_MAP_ENCODING_SRGB;
    state.uiState.exposureEv = 0.0f;
    state.uiState.whitePoint = 4.0f;
    state.uiState.lightIntensity = 5.0f;
    state.uiState.skyIntensity = 1.2f;
    state.uiState.sunIntensity = 900.0f;

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
