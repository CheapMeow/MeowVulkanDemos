#ifdef __ANDROID__

#include "asset_file.h"
#include "control_server.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
#include "rdoc_trigger.h"
#include "renderer.h"
#include "scene.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <cstring>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

#include <imgui_impl_android.h>

#define ANDROID_LOG_TAG "VulkanIndirectDrawDemo"
#define LOG_I(...) __android_log_print(ANDROID_LOG_INFO, ANDROID_LOG_TAG, __VA_ARGS__)

// 安卓入口的完整状态。实例与逻辑设备建好一次即可复用，交换链跟随 ANativeWindow 的生死重建
namespace {

struct AppState {
    VulkanContext ctx = {};
    Renderer renderer = {};
    UserInterface ui = {};
    Camera camera = {};
    UiState uiState = {};
    UiStatistics uiStatistics = {};
    TimingStore timingStore = {};
    GpuClockLockState gpuClockLockState = {};
    GpuClockMonitor gpuClockMonitor = {};

    std::vector<InstanceData> instances;
    std::vector<LightData> lights;
    std::vector<uint32_t> visibleIndices;
    MeshData mesh = {};

    uint32_t instanceCapacity = 0;
    uint32_t lightCapacity = 0;

    bool vulkanInitialized = false;  // 实例、设备、渲染器、界面都已建好，随 app 生命周期只做一次
    bool swapchainReady = false;     // 当前窗口上有可用的交换链
    bool windowValid = false;        // 当前窗口非空且没有被销毁
    bool animating = false;          // 前台且有焦点，继续画帧
    bool destroying = false;
    bool started = false;

    uint64_t frameCounter = 0;
    double previousSeconds = 0.0;
    double startSeconds = 0.0;
    double lastPrintSeconds = 0.0;

    // TCP 控制服务与分段测量状态。命令经 controlServer.pump() 在主线程执行
    ControlServer controlServer;
    bool quitRequested = false;
    bool segmentActive = false;
    double segmentStartSeconds = 0.0;
    uint32_t segmentVisibleCount = 0;
    uint32_t segmentDrawCallCount = 0;
};

// 测量分段跳过起始的预热帧
static constexpr double kWarmUpSeconds = 1.5;

// TCP 控制服务端口，脚本通过 adb reverse 把本机端口映射到设备
static constexpr uint16_t kControlPort = 21000;

// 第一块可用的窗口出现时做全量初始化：表面 -> 设备 -> 交换链 -> 网格与全部渲染资源
static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);
    LOG_I("initialized window %dx%d, swapchain extent %ux%u", ANativeWindow_getWidth(app->window),
          ANativeWindow_getHeight(app->window), state.ctx.swapchainExtent.width,
          state.ctx.swapchainExtent.height);

    const std::vector<unsigned char> meshBytes = readAssetBytes("backpack/backpack.obj");
    loadObjFromMemory(meshBytes, state.mesh);

    buildInstances(state.instanceCapacity, 8.0f, state.instances);
    state.lights.resize(state.lightCapacity);
    state.visibleIndices.resize(state.instanceCapacity);

    createRenderer(state.ctx, state.renderer, state.mesh, state.instances, state.lightCapacity);
    createUserInterface(state.ctx, state.renderer, state.ui);
    initCamera(state.camera, state.instances);

    state.uiState.drawPath = DRAW_PATH_TRADITIONAL;
    state.uiState.activeInstanceCount = 200000;
    state.uiState.activeLightCount = 64;
    state.uiState.farPlane = state.camera.farPlane;
    state.uiState.cameraMoveSpeed = state.camera.moveSpeed;
    state.camera.farPlane = state.uiState.farPlane;

    state.startSeconds = nowSeconds();
    state.previousSeconds = state.startSeconds;
    state.vulkanInitialized = true;
    state.swapchainReady = true;
    LOG_I("android renderer initialized, instances %d, light capacity %u", state.uiState.activeInstanceCount,
          state.lightCapacity);
}

// 窗口被系统销毁后重建时，设备与渲染器不变，只重建表面与交换链
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

// 呈现时发现交换链失效（旋转导致尺寸变化、内容过期），表面仍然有效，只重建交换链
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

// 交换链的尺寸必须与窗口一致。横屏启动时窗口尺寸可能晚一步才定型，
// 或者旋转后系统没有发重建事件，每帧对一次，不一致就重建交换链
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
            // ANativeWindow 到达，首次走全量初始化，之后窗口重建只重建交换链
            if (!state.vulkanInitialized) {
                initializeRendererStack(state, app);
            } else {
                state.ctx.window = app->window;
                recreateSurfaceAndSwapchain(state);
            }
            state.windowValid = true;
            // 焦点事件可能先于窗口到达，窗口就绪时强制进入绘制状态，避免一直阻塞等事件
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

// 安卓没有键盘，全部输入都是触摸事件，转给 ImGui 的安卓后端
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
        // 跳过第一帧：窗口刚建好时 delta 包含了初始化耗时
        state.started = true;
        return;
    }

    beginUserInterfaceFrame();
    buildUserInterface(state.uiState, state.uiStatistics, state.timingStore,
                       static_cast<int>(state.instanceCapacity), static_cast<int>(state.lightCapacity),
                       state.gpuClockLockState, state.gpuClockMonitor);
    endUserInterfaceFrame();

    // 渲染前先保证交换链尺寸与窗口一致，横屏输出不会被当成竖屏绘制后拉伸
    syncSwapchainToWindow(state);

    state.camera.farPlane = state.uiState.farPlane;
    state.camera.moveSpeed = state.uiState.cameraMoveSpeed;
    // 安卓端相机保持初始化时的位置与朝向，没有键盘输入

    const uint32_t activeInstanceCount = static_cast<uint32_t>(state.uiState.activeInstanceCount);
    const uint32_t activeLightCount = static_cast<uint32_t>(state.uiState.activeLightCount);
    updateLights(state.camera.position, activeLightCount, 48.0f, 110.0f, state.lights);

    const float aspectRatio = static_cast<float>(state.ctx.swapchainExtent.width) /
                              static_cast<float>(state.ctx.swapchainExtent.height);
    CameraUniform cameraUniform;
    fillCameraUniform(state.camera, aspectRatio, activeInstanceCount, state.mesh.boundsRadius, activeLightCount,
                      cameraUniform);

    FrameInput input = {};
    input.drawPath = state.uiState.drawPath;
    input.activeInstanceCount = activeInstanceCount;
    input.activeLightCount = activeLightCount;
    input.drawUserInterface = true;

    FrameStatistics statistics = {};
    const bool frameDrawn = drawFrame(state.ctx, state.renderer, state.frameCounter, input, cameraUniform,
                                      state.lights, state.instances, state.visibleIndices.data(), statistics);
    if (!frameDrawn) {
        // 交换链在获取或呈现时失效（旋转、切后台），重建后从下一帧继续
        rebuildSwapchain(state);
        return;
    }
    ++state.frameCounter;

    state.uiStatistics.visibleInstanceCount = statistics.visibleInstanceCount;
    state.uiStatistics.drawCallCount = statistics.drawCallCount;

    double timingValues[TIMING_ID_COUNT];
    timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
    timingValues[TIMING_CPU_CULL] = statistics.cpuCullMilliseconds;
    timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
    timingValues[TIMING_CPU_RECORD_CULL_DISPATCH] = statistics.cpuRecordCullDispatchMilliseconds;
    timingValues[TIMING_CPU_RECORD_GBUFFER_PASS] = statistics.cpuRecordGBufferPassMilliseconds;
    timingValues[TIMING_CPU_RECORD_LIGHTING_PASS] = statistics.cpuRecordLightingPassMilliseconds;
    timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
    timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
    timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
    timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
    const bool includeInReport =
        state.segmentActive && currentTime - state.segmentStartSeconds >= kWarmUpSeconds;
    if (includeInReport) {
        state.segmentVisibleCount = statistics.visibleInstanceCount;
        state.segmentDrawCallCount = statistics.drawCallCount;
    }
    recordFrameTimingSamples(state.timingStore, currentTime, includeInReport, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, visible %u, draw commands %u, swapchain %ux%u", fps,
              statistics.visibleInstanceCount, statistics.drawCallCount, state.ctx.swapchainExtent.width,
              state.ctx.swapchainExtent.height);
        state.lastPrintSeconds = currentTime;
    }
}

// TCP 控制命令与桌面端一致：path/instances/lights/far 改配置，
// begin/end 圈定一段测量并返回一行报告，capture 触发 RenderDoc 截帧
static void installControlHandler(AppState& state)
{
    state.controlServer.onCommand = [&state](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "path") {
            std::string value;
            stream >> value;
            DrawPath newPath = DRAW_PATH_TRADITIONAL;
            if (value == "traditional" || value == "0") {
                newPath = DRAW_PATH_TRADITIONAL;
            } else if (value == "instanced" || value == "1") {
                newPath = DRAW_PATH_INSTANCED;
            } else if (value == "indirect" || value == "2") {
                newPath = DRAW_PATH_INDIRECT;
            } else {
                return "err: unknown path";
            }
            state.uiState.drawPath = newPath;
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "instances") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(state.instanceCapacity)) {
                return "err: instances out of range";
            }
            state.uiState.activeInstanceCount = static_cast<int>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "lights") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(state.lightCapacity)) {
                return "err: lights out of range";
            }
            state.uiState.activeLightCount = static_cast<int>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "far") {
            double value = 0.0;
            if (!(stream >> value) || value <= 0.0) {
                return "err: bad far plane";
            }
            state.uiState.farPlane = static_cast<float>(value);
            resetTimingWindows(state.timingStore);
            return "ok";
        }
        if (verb == "begin") {
            state.segmentActive = true;
            state.segmentStartSeconds = nowSeconds();
            state.segmentVisibleCount = 0;
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
            const std::string row = timingReportLine(
                drawPathName(state.uiState.drawPath),
                static_cast<uint32_t>(state.uiState.activeInstanceCount), state.segmentVisibleCount,
                state.segmentDrawCallCount, state.timingStore);
            state.segmentActive = false;
            resetTimingReport(state.timingStore);
            return "row " + row;
        }
        if (verb == "capture") {
            renderdocTriggerCapture();
            return "ok";
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
    state.instanceCapacity = 1000000;
    state.lightCapacity = 256;

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
        // 处理 TCP 控制命令，quit 请求结束 activity
        state.controlServer.pump();
        if (state.quitRequested) {
            break;
        }

        // 逐条处理事件。没有事件可处理时：需要画帧就立即进入绘制（超时 0），
        // 否则阻塞等下一个事件（超时 -1）。超时值每轮都按当前状态重新计算，
        // 事件回调里 animating 由假变真之后才能立刻开始画帧
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
            // 简单的帧率上限，避免无界面限制地空转发热
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
        // 拆除全部资源后直接结束进程。activity 的 finish 需要活动回到前台才会走完
        // 销毁流程，等它既不及时又不可靠，进程自退出由系统回收即可
        _exit(0);
    }
}

#endif
