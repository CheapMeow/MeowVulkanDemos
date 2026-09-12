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

// 实例之间的水平间距、物体缩放，以及地面比起实例网格向外多伸出的比例
static const float INSTANCE_SPACING = 12.0f;
static const float INSTANCE_SCALE = 2.5f;
static const float GROUND_MARGIN = 1.2f;

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS = "instances,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    ShadowRenderer renderer = {};
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

    uint32_t instanceCapacity = 0;
    float objectCenterHeight = 0.0f;
    float objectRadius = 0.0f;
    float viewDistance = 0.0f;

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

static std::string shadowReportPrefix(uint32_t instances, uint32_t drawCommands)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "%u,%u", instances, drawCommands);
    return std::string(prefix);
}

// 由光源的两个角度与场景半径构造光源的正交投影
static void fillShadowUniform(const Camera& camera, float aspectRatio, const glm::vec3& lightDirection,
                              float sceneRadius, uint32_t shadowMapSize, bool shadowsEnabled,
                              bool pcfEnabled, ShadowSceneUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.view = matrices.view;
    outUniform.projection = matrices.projection;
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;

    const float radius = std::max(sceneRadius, 10.0f);
    const glm::vec3 center = glm::vec3(0.0f, radius * 0.1f, 0.0f);
    const glm::vec3 eye = center + lightDirection * radius * 2.5f;
    const glm::mat4 lightView = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProjection = glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 6.0f);
    // Vulkan 的裁剪空间 Y 轴朝下
    lightProjection[1][1] *= -1.0f;
    outUniform.lightViewProjection = lightProjection * lightView;

    outUniform.lightDirection = glm::vec4(lightDirection, 0.0f);
    outUniform.lightColor = glm::vec4(1.0f, 0.96f, 0.9f, 3.0f);
    outUniform.shadowParams = glm::vec4(0.0005f, 1.0f / static_cast<float>(shadowMapSize),
                                        pcfEnabled ? 1.0f : 0.0f, shadowsEnabled ? 1.0f : 0.0f);
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
    buildGroundPlaneMesh(state.groundMesh);

    state.objectCenterHeight = objectCenterHeightForMesh(state.objectMesh);
    state.objectRadius = (state.objectCenterHeight + state.objectMesh.boundsRadius) * INSTANCE_SCALE;

    buildShadowInstances(state.instanceCapacity, INSTANCE_SPACING, state.objectCenterHeight, INSTANCE_SCALE,
                         state.instances);
    const uint32_t groundInstanceIndex = static_cast<uint32_t>(state.instances.size());
    const float groundScale =
        2.0f * (shadowGridHalfExtent(state.instanceCapacity, INSTANCE_SPACING) + state.objectRadius) *
        GROUND_MARGIN;
    InstanceData groundInstance = {};
    groundInstance.positionScale = glm::vec4(0.0f, 0.0f, 0.0f, groundScale);
    state.instances.push_back(groundInstance);

    createRenderer(state.ctx, state.renderer, state.objectMesh, state.groundMesh, state.instances,
                   groundInstanceIndex, state.uiState.shadowMapSize, state.uiState.shadowMapBits);

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(state.ctx, state.renderer.mainRenderPass, caseTexts, caseTextCount, state.ui);

    // 相机固定俯视整个网格，安卓端没有键盘输入
    const float initialHalfExtent = shadowGridHalfExtent(state.uiState.activeInstanceCount, INSTANCE_SPACING);
    state.viewDistance = std::max(60.0f, initialHalfExtent * 2.6f);
    state.camera.position = glm::vec3(0.0f, state.viewDistance * 0.75f, state.viewDistance);
    state.camera.yaw = 0.0f;
    state.camera.pitch = -0.55f;
    state.camera.verticalFieldOfView = glm::radians(60.0f);
    state.camera.nearPlane = 0.5f;
    state.camera.farPlane = state.viewDistance * 6.0f;
    state.camera.moveSpeed = std::max(40.0f, initialHalfExtent);

    state.uiState.cameraMoveSpeed = state.camera.moveSpeed;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    initTimingStore(state.timingStore, caseTimingItems(), TIMING_ID_COUNT, state.startSeconds);
    state.reportHeaderColumns = std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(state.timingStore);
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android shadow renderer initialized, instances %d", state.uiState.activeInstanceCount);
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
                       static_cast<int>(state.instanceCapacity), state.gpuClockLockState,
                       state.gpuClockMonitor);
    endUserInterfaceFrame();

    syncSwapchainToWindow(state);

    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;

    const uint32_t activeInstanceCount = static_cast<uint32_t>(state.uiState.activeInstanceCount);

    const float yaw = glm::radians(state.uiState.lightYawDegrees);
    const float pitch = glm::radians(state.uiState.lightPitchDegrees);
    const glm::vec3 lightDirection =
        glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                 -std::cos(pitch) * std::cos(yaw)));

    const float sceneRadius = shadowGridHalfExtent(activeInstanceCount, INSTANCE_SPACING) + state.objectRadius;

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);

    ShadowSceneUniform sceneUniform;
    fillShadowUniform(state.camera, aspectRatio, lightDirection, sceneRadius, state.uiState.shadowMapSize,
                      state.uiState.shadowsEnabled, state.uiState.pcfEnabled, sceneUniform);

    FrameInput input = {};
    input.activeInstanceCount = activeInstanceCount;
    input.drawUserInterface = true;
    input.shadowsEnabled = state.uiState.shadowsEnabled;
    input.pcfRadius = state.uiState.pcfEnabled ? 1.0f : 0.0f;
    input.shadowMapSize = state.uiState.shadowMapSize;
    input.shadowMapBits = state.uiState.shadowMapBits;

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
    timingValues[TIMING_CPU_RECORD_SHADOW_PASS] = statistics.cpuRecordShadowPassMilliseconds;
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
        LOG_I("frame %.1f FPS, draw commands %u, swapchain %ux%u", fps, statistics.drawCallCount,
              state.ctx.swapchainExtent.width, state.ctx.swapchainExtent.height);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：instances/yaw/pitch 改配置，shadows/pcf 开关效果，
// begin/end 圈定一段测量并返回一行报告，quit 结束进程
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "instances") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(state.instanceCapacity)) {
                return "err: instances out of range";
            }
            state.uiState.activeInstanceCount = static_cast<int>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "yaw") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: bad yaw";
            }
            state.uiState.lightYawDegrees = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "pitch") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: bad pitch";
            }
            state.uiState.lightPitchDegrees = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "shadows") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: shadows takes 0 or 1";
            }
            state.uiState.shadowsEnabled = value == 1;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "pcf") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: pcf takes 0 or 1";
            }
            state.uiState.pcfEnabled = value == 1;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "shadow-size") {
            long value = 0;
            if (!(stream >> value) || value < SHADOW_MAP_MIN_SIZE || value > SHADOW_MAP_MAX_SIZE) {
                return "err: shadow-size out of range";
            }
            state.uiState.shadowMapSize = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "shadow-bits") {
            long value = 0;
            if (!(stream >> value) || (value != 8 && value != 16 && value != 32)) {
                return "err: shadow-bits takes 8, 16 or 32";
            }
            state.uiState.shadowMapBits = static_cast<uint32_t>(value);
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
                shadowReportPrefix(static_cast<uint32_t>(state.uiState.activeInstanceCount),
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
    state.instanceCapacity = 20000;
    state.uiState.activeInstanceCount = 2000;
    state.uiState.lightYawDegrees = 135.0f;
    state.uiState.lightPitchDegrees = 30.0f;
    state.uiState.shadowsEnabled = true;
    state.uiState.pcfEnabled = true;
    state.uiState.shadowMapSize = SHADOW_MAP_DEFAULT_SIZE;
    state.uiState.shadowMapBits = SHADOW_MAP_DEFAULT_BITS;

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
