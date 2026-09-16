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

const float NEAR_PLANE = 0.1f;
const float FAR_PLANE = 30.0f;

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS =
    "mode,focal_length,fnumber,focus_distance,max_radius,samples,blades,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    DofRenderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
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
    uint32_t segmentDrawCallCount = 0;
};

static constexpr double kWarmUpSeconds = 1.5;
static constexpr uint16_t kControlPort = 21000;

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[192];
    std::snprintf(prefix, sizeof(prefix), "%s,%.3f,%.1f,%.2f,%.0f,%u,%u,%u", dofModeName(state.mode),
                  state.focalLength, state.fNumber, state.focusDistance, state.maxRadius,
                  state.sampleCount, state.blades, drawCommands);
    return std::string(prefix);
}

static void fillSceneUniform(const Camera& camera, float aspectRatio, const UiState& state,
                             SceneUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.lightDirection = glm::vec4(glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f)),
                                          state.lightIntensity);
    outUniform.sceneParams = glm::vec4(state.ambient, 0.0f, 0.0f, 0.0f);
}

static void fillDofUniform(const VulkanContext& ctx, const UiState& state, DofUniform& outUniform)
{
    outUniform.cameraParams = glm::vec4(NEAR_PLANE, FAR_PLANE,
                                        static_cast<float>(ctx.swapchainExtent.width),
                                        static_cast<float>(ctx.swapchainExtent.height));
    outUniform.lensParams = glm::vec4(state.focalLength, state.fNumber, state.focusDistance,
                                      state.maxRadius);
    outUniform.modeParams = glm::vec4(static_cast<float>(state.mode),
                                      static_cast<float>(state.sampleCount), state.jitter,
                                      static_cast<float>(state.blades));
    outUniform.miscParams = glm::vec4(state.exposure, 0.0f, 0.0f, 0.0f);
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

    // 相机固定在初始位置，安卓端没有键盘输入
    state.camera.position = glm::vec3(0.0f, 0.35f, 0.5f);
    state.camera.yaw = 0.0f;
    state.camera.pitch = -0.10f;
    state.camera.verticalFieldOfView = glm::radians(55.0f);
    state.camera.nearPlane = NEAR_PLANE;
    state.camera.farPlane = FAR_PLANE;
    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android depth of field renderer initialized");
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

    SceneUniform sceneUniform;
    fillSceneUniform(state.camera, aspectRatio, state.uiState, sceneUniform);
    DofUniform dofUniform;
    fillDofUniform(state.ctx, state.uiState, dofUniform);

    FrameInput input = {};
    input.drawUserInterface = true;

    FrameStatistics statistics = {};
    const bool frameDrawn = drawFrame(state.ctx, state.renderer, state.frameCounter, input,
                                      sceneUniform, dofUniform, statistics);
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
    timingValues[TIMING_CPU_RECORD_POST] = statistics.cpuRecordPostMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    timingValues[TIMING_GPU_SCENE] = statistics.gpuSceneMilliseconds;
    timingValues[TIMING_GPU_POST] = statistics.gpuPostMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    if (includeInReport) {
        state.segmentDrawCallCount = statistics.drawCallCount;
    }
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, mode %s, focus %.2f, f/%.1f", fps, dofModeName(state.uiState.mode),
              state.uiState.focusDistance, state.uiState.fNumber);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：mode/samples/blades/focal/fnumber/focus/max-radius/jitter/exposure/light/ambient
// 改配置，begin/end 圈定一段测量，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "mode") {
            std::string value;
            if (!(stream >> value) || (value != "off" && value != "gaussian" && value != "disk")) {
                return "err: mode takes off, gaussian or disk";
            }
            state.uiState.mode = value == "off" ? DOF_MODE_OFF
                                 : value == "gaussian" ? DOF_MODE_GAUSSIAN
                                                       : DOF_MODE_DISK;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || value < 4 || value > 512) {
                return "err: samples takes an integer between 4 and 512";
            }
            state.uiState.sampleCount = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "blades") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value > 12) {
                return "err: blades takes an integer between 0 and 12";
            }
            state.uiState.blades = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "focal" || verb == "fnumber" || verb == "focus" || verb == "max-radius" ||
            verb == "jitter" || verb == "exposure" || verb == "light" || verb == "ambient") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "focal") {
                state.uiState.focalLength = value;
            } else if (verb == "fnumber") {
                state.uiState.fNumber = value;
            } else if (verb == "focus") {
                state.uiState.focusDistance = value;
            } else if (verb == "max-radius") {
                state.uiState.maxRadius = value;
            } else if (verb == "jitter") {
                state.uiState.jitter = value;
            } else if (verb == "exposure") {
                state.uiState.exposure = value;
            } else if (verb == "light") {
                state.uiState.lightIntensity = value;
            } else {
                state.uiState.ambient = value;
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
    state.uiState.cameraMoveSpeed = 1.0f;
    state.uiState.mode = DOF_MODE_DISK;
    state.uiState.focalLength = 0.05f;
    state.uiState.fNumber = 1.4f;
    state.uiState.focusDistance = 2.9f;
    state.uiState.maxRadius = 32.0f;
    state.uiState.sampleCount = 128;
    state.uiState.jitter = 0.5f;
    state.uiState.blades = 0;
    state.uiState.exposure = 1.0f;
    state.uiState.lightIntensity = 0.9f;
    state.uiState.ambient = 0.08f;

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
