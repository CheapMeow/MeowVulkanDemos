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

// 报告里 case 自己的前几列
const char* const REPORT_HEADER_COLUMNS =
    "lighting,distribution,geometry,fresnel,multiscatter,roughness,metallic,base_color,draw_commands";

struct AppState {
    VulkanContext ctx = {};
    BrdfRenderer renderer = {};
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
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix), "%s,%s,%s,%s,%d,%.3f,%.2f,%.2f,%u",
                  brdfLightingName(state.lighting), brdfDistributionName(state.distribution),
                  brdfGeometryName(state.geometry), brdfFresnelName(state.fresnel),
                  state.multiScattering ? 1 : 0, state.roughness, state.metallic, state.baseColor,
                  drawCommands);
    return std::string(prefix);
}

static void fillUniform(const Camera& camera, float aspectRatio, const UiState& state,
                        const BrdfRenderer& renderer, BrdfUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;
    outUniform.lightDirection = glm::vec4(glm::normalize(glm::vec3(0.4f, 0.6f, 0.7f)),
                                          state.lightIntensity);
    outUniform.lightColor = glm::vec4(1.0f, 0.98f, 0.95f, state.environmentRadiance);
    outUniform.material = glm::vec4(state.roughness, state.metallic, state.baseColor, 0.0f);
    outUniform.options = glm::vec4(static_cast<float>(state.lighting),
                                   static_cast<float>(state.distribution),
                                   static_cast<float>(state.geometry),
                                   state.multiScattering ? 1.0f : 0.0f);
    outUniform.miscParams = glm::vec4(static_cast<float>(state.fresnel),
                                      static_cast<float>(state.furnaceSamples), state.exposure, 0.0f);
    std::memcpy(outUniform.averageAlbedo, renderer.averageAlbedo, sizeof(outUniform.averageAlbedo));
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
    state.camera.position = glm::vec3(0.0f, 0.0f, 3.2f);
    state.camera.yaw = 0.0f;
    state.camera.pitch = 0.0f;
    state.camera.verticalFieldOfView = glm::radians(50.0f);
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
    LOG_I("android microfacet brdf renderer initialized");
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

    BrdfUniform uniform;
    fillUniform(state.camera, aspectRatio, state.uiState, state.renderer, uniform);

    FrameInput input = {};
    input.drawUserInterface = true;
    input.distribution = state.uiState.distribution;

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
        LOG_I("frame %.1f FPS, lighting %s, distribution %s, roughness %.3f", fps,
              brdfLightingName(state.uiState.lighting), brdfDistributionName(state.uiState.distribution),
              state.uiState.roughness);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令：lighting/distribution/geometry/fresnel/multiscatter/samples/roughness/
// metallic/base-color/light/environment/exposure 改配置，begin/end 圈定一段测量，quit 退出
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "lighting") {
            std::string value;
            if (!(stream >> value) || (value != "directional" && value != "furnace" && value != "table")) {
                return "err: lighting takes directional, furnace or table";
            }
            state.uiState.lighting = value == "directional" ? BRDF_LIGHTING_DIRECTIONAL
                                     : value == "furnace"   ? BRDF_LIGHTING_FURNACE
                                                            : BRDF_LIGHTING_TABLE;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "distribution") {
            std::string value;
            if (!(stream >> value) ||
                (value != "ggx" && value != "beckmann" && value != "blinn_phong")) {
                return "err: distribution takes ggx, beckmann or blinn_phong";
            }
            state.uiState.distribution = value == "ggx"        ? BRDF_DISTRIBUTION_GGX
                                         : value == "beckmann" ? BRDF_DISTRIBUTION_BECKMANN
                                                               : BRDF_DISTRIBUTION_BLINN_PHONG;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "geometry") {
            std::string value;
            if (!(stream >> value) || (value != "smith" && value != "schlick" && value != "none")) {
                return "err: geometry takes smith, schlick or none";
            }
            state.uiState.geometry = value == "smith"      ? BRDF_GEOMETRY_SMITH
                                     : value == "schlick"  ? BRDF_GEOMETRY_SCHLICK
                                                           : BRDF_GEOMETRY_NONE;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "fresnel") {
            std::string value;
            if (!(stream >> value) || (value != "schlick" && value != "constant")) {
                return "err: fresnel takes schlick or constant";
            }
            state.uiState.fresnel =
                value == "constant" ? BRDF_FRESNEL_CONSTANT : BRDF_FRESNEL_SCHLICK;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "multiscatter") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: multiscatter takes 0 or 1";
            }
            state.uiState.multiScattering = value == 1;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || value < 16 || value > 256) {
                return "err: samples takes an integer between 16 and 256";
            }
            state.uiState.furnaceSamples = static_cast<uint32_t>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "roughness" || verb == "metallic" || verb == "base-color" || verb == "light" ||
            verb == "environment" || verb == "exposure") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "roughness") {
                state.uiState.roughness = value;
            } else if (verb == "metallic") {
                state.uiState.metallic = value;
            } else if (verb == "base-color") {
                state.uiState.baseColor = value;
            } else if (verb == "light") {
                state.uiState.lightIntensity = value;
            } else if (verb == "environment") {
                state.uiState.environmentRadiance = value;
            } else {
                state.uiState.exposure = value;
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
    state.uiState.lighting = BRDF_LIGHTING_DIRECTIONAL;
    state.uiState.distribution = BRDF_DISTRIBUTION_GGX;
    state.uiState.geometry = BRDF_GEOMETRY_SMITH;
    state.uiState.fresnel = BRDF_FRESNEL_SCHLICK;
    state.uiState.multiScattering = true;
    state.uiState.roughness = 0.4f;
    state.uiState.metallic = 0.0f;
    state.uiState.baseColor = 0.7f;
    state.uiState.lightIntensity = 3.0f;
    state.uiState.environmentRadiance = 0.5f;
    state.uiState.furnaceSamples = 128;
    state.uiState.exposure = 1.0f;

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
