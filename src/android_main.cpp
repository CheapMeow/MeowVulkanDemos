#ifdef __ANDROID__

#include "asset_file.h"
#include "gpu_clock_lock.h"
#include "obj_loader.h"
#include "renderer.h"
#include "scene.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <cstring>
#include <thread>
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
};

// 第一块可用的窗口出现时做全量初始化：表面 -> 设备 -> 交换链 -> 网格与全部渲染资源
static void initializeRendererStack(AppState& state, android_app* app)
{
    createWindowSurface(state.ctx, app->window);
    createGraphicsDevice(state.ctx);
    createSwapchain(state.ctx);

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
    recordFrameTimingSamples(state.timingStore, currentTime, true, timingValues);

    if (currentTime - state.lastPrintSeconds >= 2.0) {
        const TimingWindow& frameWindow = state.timingStore.window[TIMING_FRAME];
        const double fps = frameWindow.mean > 0.0 ? 1000.0 / frameWindow.mean : 0.0;
        LOG_I("frame %.1f FPS, visible %u, draw commands %u", fps, statistics.visibleInstanceCount,
              statistics.drawCallCount);
        state.lastPrintSeconds = currentTime;
    }
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

    const double frameBudgetSeconds = 1.0 / 60.0;

    while (!state.destroying) {
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

    teardownSwapchain(state);
    VK_CHECK(vkDeviceWaitIdle(state.ctx.device));
    if (state.vulkanInitialized) {
        destroyUserInterface(state.ctx, state.ui);
        destroyRenderer(state.ctx, state.renderer);
    }
    destroyVulkanContext(state.ctx);
}

#endif
