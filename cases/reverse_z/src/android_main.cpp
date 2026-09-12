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

#include <glm/gtc/matrix_transform.hpp>

#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

#include <imgui_impl_android.h>

#define ANDROID_LOG_TAG "MeowVulkanDemo"
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)

namespace {

// 相机放在地面正上方，俯角 20 度朝远处看
const float CAMERA_HEIGHT = 66.0f;
const float CAMERA_PITCH_RADIANS = -0.349066f;

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS = "objects,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    ReverseZRenderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
    UiState uiState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};
    std::string reportHeaderColumns;

    std::vector<InstanceData> instances;
    MeshData objectMesh = {};
    MeshData groundMesh = {};

    uint32_t objectCapacity = 0;
    uint32_t objectInstanceOffset = 0;

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

static std::string reverseZReportPrefix(uint32_t objects, uint32_t drawCommands)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "%u,%u", objects, drawCommands);
    return std::string(prefix);
}

// 相机矩阵。Reverse-Z 把近平面映射到 1、远平面映射到 0，做法是把透视投影的两个平面
// 距离互换：公共库的 fillCameraMatrices 按 Camera 里的近远平面生成矩阵，交换这两个平面
// 就得到反向的映射，视图矩阵、世界位置与投影均不受影响
static void fillSceneUniform(const Camera& camera, float aspectRatio, bool reverseZ, float groundOffset,
                             SceneUniform& outUniform)
{
    CameraMatrices matrices;
    if (reverseZ) {
        Camera reversedCamera = camera;
        std::swap(reversedCamera.nearPlane, reversedCamera.farPlane);
        fillCameraMatrices(reversedCamera, aspectRatio, matrices);
    } else {
        fillCameraMatrices(camera, aspectRatio, matrices);
    }

    outUniform.view = matrices.view;
    outUniform.projection = matrices.projection;
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.lightDirection = glm::vec4(glm::normalize(glm::vec3(0.42f, 0.78f, 0.46f)), 0.0f);
    outUniform.groundParams = glm::vec4(groundOffset, 0.0f, 0.0f, 0.0f);
}

static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    const std::vector<unsigned char> meshBytes = readAssetBytes("backpack/backpack.obj");
    loadObjFromMemory(meshBytes, state.objectMesh);
    buildGroundStripMesh(state.groundMesh);

    const float objectCenterHeight = objectCenterHeightForMesh(state.objectMesh);

    buildGroundInstances(state.instances);
    state.objectInstanceOffset = static_cast<uint32_t>(state.instances.size());

    std::vector<InstanceData> objectInstances;
    buildObjectInstances(state.objectCapacity, objectCenterHeight, objectInstances);
    state.instances.insert(state.instances.end(), objectInstances.begin(), objectInstances.end());

    createRenderer(state.ctx, state.renderer, state.objectMesh, state.groundMesh, state.instances,
                   state.objectInstanceOffset, state.uiState.reverseZ);

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.renderPass, caseTexts, caseTextCount, state.ui);

    state.camera.position = glm::vec3(0.0f, CAMERA_HEIGHT, 0.0f);
    state.camera.yaw = 0.0f;
    state.camera.pitch = CAMERA_PITCH_RADIANS;
    state.camera.verticalFieldOfView = glm::radians(60.0f);
    state.camera.nearPlane = state.uiState.nearPlane;
    state.camera.farPlane = state.uiState.farPlane;
    state.camera.moveSpeed = 60.0f;
    state.uiState.cameraMoveSpeed = state.camera.moveSpeed;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns = std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android reverse-z renderer initialized, objects %d", state.uiState.activeObjectCount);
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
    buildUserInterface(state.uiState, state.uiStatistics, state.timingStore,
                       static_cast<int>(state.objectCapacity), state.gpuClockLockState,
                       state.gpuClockMonitor);
    endUserInterfaceFrame();

    syncSwapchainToWindow(state);

    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;
    state.camera.nearPlane = state.uiState.nearPlane;
    state.camera.farPlane = state.uiState.farPlane;

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);

    SceneUniform sceneUniform;
    fillSceneUniform(state.camera, aspectRatio, state.uiState.reverseZ, state.uiState.groundOffset,
                     sceneUniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.reverseZ = state.uiState.reverseZ;
    input.activeObjectCount = static_cast<uint32_t>(state.uiState.activeObjectCount);

    FrameStatistics statistics = {};
    const bool frameDrawn = drawFrame(state.ctx, state.renderer, state.frameCounter, input, sceneUniform,
                                      statistics);
    if (!frameDrawn) {
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.drawCallCount = statistics.drawCallCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_MAIN_PASS] = statistics.cpuRecordMainPassMilliseconds;
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
        LOG_I("frame %.1f FPS, draw commands %u, depth %s, swapchain %ux%u", fps, statistics.drawCallCount,
              state.uiState.reverseZ ? "reverse-z" : "standard", state.ctx.swapchainExtent.width,
              state.ctx.swapchainExtent.height);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：objects/near/far/ground-offset 改配置，reverse-z 切换深度模式，
// begin/end 圈定一段测量并返回一行报告，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "objects") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value > static_cast<long>(state.objectCapacity)) {
                return "err: objects out of range";
            }
            state.uiState.activeObjectCount = static_cast<int>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "reverse-z") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: reverse-z takes 0 or 1";
            }
            state.uiState.reverseZ = value == 1;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "near") {
            double value = 0.0;
            if (!(stream >> value) || value < NEAR_PLANE_MIN || value > NEAR_PLANE_MAX) {
                return "err: near out of range";
            }
            state.uiState.nearPlane = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "far") {
            double value = 0.0;
            if (!(stream >> value) || value < FAR_PLANE_MIN || value > FAR_PLANE_MAX) {
                return "err: far out of range";
            }
            state.uiState.farPlane = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "ground-offset") {
            double value = 0.0;
            if (!(stream >> value) || value < GROUND_OFFSET_MIN || value > GROUND_OFFSET_MAX) {
                return "err: ground-offset out of range";
            }
            state.uiState.groundOffset = static_cast<float>(value);
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
                reverseZReportPrefix(static_cast<uint32_t>(state.uiState.activeObjectCount),
                                     state.segmentDrawCallCount) +
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
    state.objectCapacity = 64;
    state.uiState.activeObjectCount = 12;
    state.uiState.reverseZ = false;
    state.uiState.nearPlane = 0.1f;
    state.uiState.farPlane = 20000.0f;
    state.uiState.groundOffset = 0.01f;

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
