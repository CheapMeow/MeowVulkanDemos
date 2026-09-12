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
#include <thread>
#include <unistd.h>
#include <vector>

#include <imgui_impl_android.h>

#define ANDROID_LOG_TAG "MeowVulkanDemo"
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)

namespace {

// 地面的半边长与相机初始位置
static const float GROUND_HALF_EXTENT = 60.0f;

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS = "view,mipmapped,anisotropy,lod_bias,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    LodRenderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
    UiState uiState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};
    std::string reportHeaderColumns;

    MeshData groundMesh = {};

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

static const char* viewName(uint32_t mode)
{
    if (mode == LOD_VIEW_LOD) {
        return "lod";
    }
    if (mode == LOD_VIEW_FOOTPRINT) {
        return "footprint";
    }
    return "normal";
}

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[128];
    std::snprintf(prefix, sizeof(prefix), "%s,%d,%.0f,%.2f,%u", viewName(state.viewMode),
                  state.mipmapped ? 1 : 0, static_cast<double>(state.maxAnisotropy),
                  static_cast<double>(state.lodBias), drawCommands);
    return std::string(prefix);
}

static void applyCameraPitch(Camera& camera, float pitchDegrees)
{
    camera.pitch = -glm::radians(pitchDegrees);
}

static void fillSceneUniform(const Camera& camera, float aspectRatio, uint32_t viewMode, uint32_t viewportWidth,
                             LodSceneUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.options = glm::vec4(static_cast<float>(viewMode), 0.0f, static_cast<float>(viewportWidth), 0.0f);
}

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    buildGroundQuadMesh(GROUND_HALF_EXTENT, state.groundMesh);

    createRenderer(state.ctx, state.renderer, state.groundMesh);

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.renderPass, caseTexts, caseTextCount, state.ui);

    // 相机固定在初始位置，安卓端没有键盘输入
    state.camera.position = glm::vec3(0.0f, 40.0f, 90.0f);
    state.camera.yaw = 0.0f;
    applyCameraPitch(state.camera, state.uiState.cameraPitchDegrees);
    state.camera.verticalFieldOfView = glm::radians(60.0f);
    state.camera.nearPlane = 0.5f;
    state.camera.farPlane = 1000.0f;
    state.camera.moveSpeed = 20.0f;

    state.uiState.cameraMoveSpeed = state.camera.moveSpeed;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns = std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android texture lod renderer initialized");
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

    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;
    applyCameraPitch(state.camera, state.uiState.cameraPitchDegrees);

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);

    LodSceneUniform sceneUniform;
    fillSceneUniform(state.camera, aspectRatio, state.uiState.viewMode, state.ctx.swapchainExtent.width,
                     sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.viewMode = state.uiState.viewMode;
    input.sampler.mipmapped = state.uiState.mipmapped;
    input.sampler.lodBias = state.uiState.lodBias;
    input.sampler.maxAnisotropy = state.uiState.maxAnisotropy;

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
        LOG_I("frame %.1f FPS, view %s, mipmap %d, anisotropy %.0f", fps, viewName(state.uiState.viewMode),
              state.uiState.mipmapped ? 1 : 0, static_cast<double>(state.uiState.maxAnisotropy));
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：view/mipmap/anisotropy/lod-bias/pitch 改配置，
// begin/end 圈定一段测量并返回一行报告，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "view") {
            std::string value;
            if (!(stream >> value) || (value != "normal" && value != "lod" && value != "footprint")) {
                return "err: view takes normal, lod or footprint";
            }
            state.uiState.viewMode = value == "lod" ? LOD_VIEW_LOD
                                     : value == "footprint" ? LOD_VIEW_FOOTPRINT
                                                            : LOD_VIEW_NORMAL;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "mipmap") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: mipmap takes 0 or 1";
            }
            state.uiState.mipmapped = value == 1;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "anisotropy") {
            double value = 0.0;
            if (!(stream >> value) || value < 1.0 || value > 16.0) {
                return "err: anisotropy out of range";
            }
            state.uiState.maxAnisotropy = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "lod-bias") {
            double value = 0.0;
            if (!(stream >> value) || value < -2.0 || value > 2.0) {
                return "err: lod-bias out of range";
            }
            state.uiState.lodBias = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "pitch") {
            double value = 0.0;
            if (!(stream >> value) || value < 8.0 || value > 75.0) {
                return "err: pitch out of range";
            }
            state.uiState.cameraPitchDegrees = static_cast<float>(value);
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
            const std::string row =
                reportPrefix(state.uiState, state.segmentDrawCallCount) + timingReportValueColumns(state.timingStore);
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
    state.uiState.cameraPitchDegrees = 26.0f;
    state.uiState.mipmapped = true;
    state.uiState.lodBias = 0.0f;
    state.uiState.maxAnisotropy = 1.0f;
    state.uiState.viewMode = LOD_VIEW_NORMAL;

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
