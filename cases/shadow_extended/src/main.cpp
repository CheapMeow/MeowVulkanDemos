#include "asset_file.h"
#include "case_ui.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 实例之间的水平间距、物体缩放，以及地面比起实例网格向外多伸出的比例
static const float INSTANCE_SPACING = 12.0f;
static const float INSTANCE_SCALE = 2.5f;
static const float GROUND_MARGIN = 1.2f;

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS =
    "cascades,map_size,value_mode,coord_mode,pcss,draw_commands";

static std::string reportPrefix(const UiState& state, uint32_t drawCommands)
{
    char prefix[160];
    std::snprintf(prefix, sizeof(prefix), "%u,%u,%s,%s,%d,%u", state.cascadeCount, state.shadowMapSize,
                  shadowValueModeName(state.valueMode), state.coordMode == SHADOW_COORD_VERTEX ? "vertex" : "fragment",
                  state.blockerRadius > 0.5f ? 1 : 0, drawCommands);
    return std::string(prefix);
}

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建。
// 抓帧缓冲的大小也由交换链尺寸决定，抓帧还没发生时一并重建
static void rebuildSwapchainResources(VulkanContext& ctx, ShadowRenderer& renderer, GpuBuffer& captureBuffer,
                                      bool capturePending)
{
    recreateSwapchain(ctx);
    recreateSwapchainTargets(ctx, renderer);

    if (capturePending) {
        if (captureBuffer.buffer != VK_NULL_HANDLE) {
            destroyBuffer(ctx, captureBuffer);
        }
        createCaptureBuffer(ctx, captureBuffer);
    }
}

// 实用的级联划分：对数划分与均匀划分各占一半，兼顾近处的精度与远处的覆盖
static void computeCascadeSplits(const Camera& camera, uint32_t cascadeCount, float* outSplits)
{
    const float nearPlane = camera.nearPlane;
    const float farPlane = camera.farPlane;
    for (uint32_t i = 0; i < cascadeCount; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
        const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
        const float uniformSplit = nearPlane + (farPlane - nearPlane) * p;
        outSplits[i] = 0.5f * logSplit + 0.5f * uniformSplit;
    }
}

// 每一级用包围球拟合：取这一段视锥沿视线方向的中点作为球心，
// 球半径覆盖这一段视锥的横截面，正交视锥按球半径开，因此与光源方向无关
static void fillCascadeMatrix(const Camera& camera, const glm::vec3& lightDirection, float splitNear,
                              float splitFar, float aspectRatio, float& outRadius, glm::mat4& outMatrix)
{
    const glm::vec3 forward = cameraForward(camera);

    const float tanHalfVertical = std::tan(camera.verticalFieldOfView * 0.5f);
    const float tanHalfHorizontal = tanHalfVertical * aspectRatio;
    const float cornerScale = std::sqrt(1.0f + tanHalfVertical * tanHalfVertical +
                                        tanHalfHorizontal * tanHalfHorizontal);
    const float radius = 0.5f * (splitFar - splitNear) * cornerScale;
    outRadius = radius;

    const glm::vec3 center = camera.position + forward * (0.5f * (splitNear + splitFar));
    const float distance = radius * 2.0f;
    const glm::vec3 eye = center + lightDirection * distance;
    const glm::mat4 lightView = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProjection = glm::ortho(-radius, radius, -radius, radius, 0.1f, distance + radius * 2.0f);
    // Vulkan 的裁剪空间 Y 轴朝下
    lightProjection[1][1] *= -1.0f;
    outMatrix = lightProjection * lightView;
}

// 由相机与光源方向构造各级的正交投影，并填满整个场景常量
static void fillShadowUniform(const Camera& camera, float aspectRatio, const glm::vec3& lightDirection,
                              const ShadowOptions& shadowOptions, const UiState& state,
                              ShadowSceneUniform& outUniform)
{
    CameraMatrices matrices;
    fillCameraMatrices(camera, aspectRatio, matrices);
    outUniform.view = matrices.view;
    outUniform.projection = matrices.projection;
    outUniform.viewProjection = matrices.viewProjection;
    outUniform.cameraPosition = matrices.cameraPosition;

    float splits[MAX_CASCADE_COUNT] = { 0.0f, 0.0f, 0.0f, 0.0f };
    computeCascadeSplits(camera, shadowOptions.cascadeCount, splits);

    float splitNear = camera.nearPlane;
    for (uint32_t i = 0; i < MAX_CASCADE_COUNT; ++i) {
        const float splitFar = i < shadowOptions.cascadeCount ? splits[i] : splits[shadowOptions.cascadeCount - 1];
        float radius = 10.0f;
        fillCascadeMatrix(camera, lightDirection, splitNear, splitFar, aspectRatio, radius,
                          outUniform.lightViewProjection[i]);
        splitNear = splitFar;
    }

    outUniform.lightDirection = glm::vec4(lightDirection, 0.0f);
    outUniform.lightColor = glm::vec4(1.0f, 0.96f, 0.9f, 3.0f);
    outUniform.shadowParams = glm::vec4(
        state.shadowDepthOffset, 1.0f / static_cast<float>(shadowOptions.mapSize),
        state.pcfEnabled ? 1.0f : 0.0f, state.shadowsEnabled ? 1.0f : 0.0f);
    outUniform.shadowOptions = glm::vec4(state.shadowNormalLift ? 1.0f : 0.0f,
                                         state.shadowSlopeBias ? 1.0f : 0.0f,
                                         2.0f / static_cast<float>(shadowOptions.mapSize), 0.0f);
    outUniform.cascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);
    outUniform.featureOptions =
        glm::vec4(static_cast<float>(shadowOptions.cascadeCount), state.cascadeBlend ? 1.0f : 0.0f,
                  state.blockerRadius, state.penumbraScale);
    outUniform.viewOptions = glm::vec4(static_cast<float>(state.valueMode),
                                       static_cast<float>(state.coordMode),
                                       static_cast<float>(state.viewMode), camera.farPlane);
}

int main(int argc, char** argv)
{
    configureConsoleEncoding();

    uint32_t initialInstanceCount = 2000;
    double autoExitSeconds = 0.0;
    std::string capturePath;
    std::string reportPath;
    bool interfaceEnabled = true;
    uint16_t controlPort = 0;
    uint32_t requestedCoreClockMHz = 0;
    uint32_t requestedMemoryClockMHz = 0;
    float lightYawDegrees = 135.0f;
    float lightPitchDegrees = 30.0f;
    bool shadowsEnabled = true;
    bool pcfEnabled = true;
    uint32_t shadowMapSize = SHADOW_MAP_DEFAULT_SIZE;
    uint32_t cascadeCount = 1;
    bool cascadeBlend = true;
    uint32_t valueMode = SHADOW_VALUE_DEPTH;
    uint32_t coordMode = SHADOW_COORD_FRAGMENT;
    uint32_t viewMode = SHADOW_VIEW_FINAL;
    float blockerRadius = 0.0f;
    float penumbraScale = 1.0f;
    bool normalLift = true;
    bool slopeBias = true;
    float shadowDepthOffset = 0.0005f;
    bool groundCaster = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instances") == 0 && i + 1 < argc) {
            initialInstanceCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            autoExitSeconds = std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--no-interface") == 0) {
            interfaceEnabled = false;
        } else if (std::strcmp(argv[i], "--no-shadows") == 0) {
            shadowsEnabled = false;
        } else if (std::strcmp(argv[i], "--no-pcf") == 0) {
            pcfEnabled = false;
        } else if (std::strcmp(argv[i], "--shadow-size") == 0 && i + 1 < argc) {
            shadowMapSize = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--cascades") == 0 && i + 1 < argc) {
            cascadeCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--no-cascade-blend") == 0) {
            cascadeBlend = false;
        } else if (std::strcmp(argv[i], "--vsm") == 0) {
            valueMode = SHADOW_VALUE_VSM;
        } else if (std::strcmp(argv[i], "--vertex-coords") == 0) {
            coordMode = SHADOW_COORD_VERTEX;
        } else if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            const char* value = argv[i + 1];
            if (std::strcmp(value, "final") == 0) {
                viewMode = SHADOW_VIEW_FINAL;
            } else if (std::strcmp(value, "cascades") == 0) {
                viewMode = SHADOW_VIEW_CASCADES;
            } else if (std::strcmp(value, "visibility") == 0) {
                viewMode = SHADOW_VIEW_VISIBILITY;
            } else {
                FATAL("--view takes final, cascades or visibility");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--blocker-radius") == 0 && i + 1 < argc) {
            blockerRadius = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--penumbra") == 0 && i + 1 < argc) {
            penumbraScale = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--no-normal-lift") == 0) {
            normalLift = false;
        } else if (std::strcmp(argv[i], "--no-slope-bias") == 0) {
            slopeBias = false;
        } else if (std::strcmp(argv[i], "--depth-offset") == 0 && i + 1 < argc) {
            shadowDepthOffset = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--ground-caster") == 0) {
            groundCaster = true;
        } else if (std::strcmp(argv[i], "--light-yaw") == 0 && i + 1 < argc) {
            lightYawDegrees = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--light-pitch") == 0 && i + 1 < argc) {
            lightPitchDegrees = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--control-port") == 0 && i + 1 < argc) {
            controlPort = static_cast<uint16_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--core-clock") == 0 && i + 1 < argc) {
            requestedCoreClockMHz = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--memory-clock") == 0 && i + 1 < argc) {
            requestedMemoryClockMHz = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        }
    }

    if (shadowMapSize < SHADOW_MAP_MIN_SIZE || shadowMapSize > SHADOW_MAP_MAX_SIZE) {
        FATAL("--shadow-size must be between %u and %u", SHADOW_MAP_MIN_SIZE, SHADOW_MAP_MAX_SIZE);
    }
    if (cascadeCount != 1 && cascadeCount != 2 && cascadeCount != 4) {
        FATAL("--cascades takes 1, 2 or 4");
    }
    if (shadowDepthOffset < 0.0f || shadowDepthOffset > 0.004f) {
        FATAL("--depth-offset must be between 0 and 0.004");
    }
    if (blockerRadius < 0.0f || blockerRadius > 8.0f) {
        FATAL("--blocker-radius must be between 0 and 8");
    }

    // 与 Vulkan、窗口无关，尽早探测，探测不到就在面板里如实显示，不阻止程序继续运行
    GpuClockLockState gpuClockLockState;
    detectGpuClockLockState(gpuClockLockState);

    const bool coreClockRequested = requestedCoreClockMHz > 0;
    const bool memoryClockRequested = requestedMemoryClockMHz > 0;
    if (coreClockRequested != memoryClockRequested) {
        FATAL("--core-clock and --memory-clock must be given together");
    }
    if (coreClockRequested) {
        requestGpuClockLockFromCommandLine(gpuClockLockState, requestedCoreClockMHz, requestedMemoryClockMHz);
    }

    GpuClockMonitor gpuClockMonitor;

    const uint32_t instanceCapacity = std::max(20000u, initialInstanceCount);

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit failed");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("no Vulkan loader is available in this environment");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Cascaded Shadow Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    MeshData objectMesh;
    loadObjFromMemory(readAssetBytes("backpack/backpack.obj"), objectMesh);

    MeshData groundMesh;
    buildGroundPlaneMesh(groundMesh);

    const float objectCenterHeight = objectCenterHeightForMesh(objectMesh);
    const float objectRadius = (objectCenterHeight + objectMesh.boundsRadius) * INSTANCE_SCALE;

    std::vector<InstanceData> instances;
    buildShadowInstances(instanceCapacity, INSTANCE_SPACING, objectCenterHeight, INSTANCE_SCALE, instances);

    const uint32_t groundInstanceIndex = static_cast<uint32_t>(instances.size());
    const float groundScale = 2.0f * (shadowGridHalfExtent(instanceCapacity, INSTANCE_SPACING) + objectRadius) *
                              GROUND_MARGIN;
    InstanceData groundInstance = {};
    groundInstance.positionScale = glm::vec4(0.0f, 0.0f, 0.0f, groundScale);
    groundInstance.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    instances.push_back(groundInstance);

    ShadowOptions shadowOptions = {};
    shadowOptions.mapSize = shadowMapSize;
    shadowOptions.cascadeCount = cascadeCount;
    shadowOptions.cascadeBlend = cascadeBlend;
    shadowOptions.normalLift = normalLift;
    shadowOptions.slopeBias = slopeBias;
    shadowOptions.depthOffset = shadowDepthOffset;
    shadowOptions.groundCaster = groundCaster;
    shadowOptions.valueMode = valueMode;
    shadowOptions.coordMode = coordMode;
    shadowOptions.blockerRadius = blockerRadius;
    shadowOptions.penumbraScale = penumbraScale;

    ShadowRenderer renderer = {};
    createRenderer(ctx, renderer, objectMesh, groundMesh, instances, groundInstanceIndex, shadowOptions);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.mainRenderPass, caseTexts, caseTextCount, ui);

    Camera camera = {};
    const float initialHalfExtent = shadowGridHalfExtent(initialInstanceCount, INSTANCE_SPACING);
    // 远平面拉长到网格的六倍，级联才有分段的必要
    const float viewDistance = std::max(60.0f, initialHalfExtent * 2.6f);
    camera.position = glm::vec3(0.0f, viewDistance * 0.75f, viewDistance);
    camera.yaw = 0.0f;
    camera.pitch = -0.55f;
    camera.verticalFieldOfView = glm::radians(60.0f);
    camera.nearPlane = 0.5f;
    camera.farPlane = viewDistance * 12.0f;
    camera.moveSpeed = std::max(40.0f, initialHalfExtent);

    UiState uiState = {};
    uiState.activeInstanceCount = static_cast<int>(initialInstanceCount);
    uiState.cameraMoveSpeed = camera.moveSpeed;
    uiState.lightYawDegrees = lightYawDegrees;
    uiState.lightPitchDegrees = lightPitchDegrees;
    uiState.shadowsEnabled = shadowsEnabled;
    uiState.pcfEnabled = pcfEnabled;
    uiState.shadowMapSize = shadowMapSize;
    uiState.cascadeCount = cascadeCount;
    uiState.cascadeBlend = cascadeBlend;
    uiState.shadowNormalLift = normalLift;
    uiState.shadowSlopeBias = slopeBias;
    uiState.shadowDepthOffset = shadowDepthOffset;
    uiState.shadowGroundCaster = groundCaster;
    uiState.valueMode = valueMode;
    uiState.coordMode = coordMode;
    uiState.viewMode = viewMode;
    uiState.blockerRadius = blockerRadius;
    uiState.penumbraScale = penumbraScale;

    UiStatistics uiStatistics = {};

    uint64_t frameCounter = 0;
    double previousTime = nowSeconds();
    double lastPrintTime = previousTime;
    const double startTime = previousTime;

    TimingStore timingStore;
    initTimingStore(timingStore, caseTimingItems(), TIMING_ID_COUNT, startTime);
    const std::string reportHeaderColumns =
        std::string(REPORT_HEADER_COLUMNS) + timingReportHeaderColumns(timingStore);

    // 测量报告跳过起始的预热阶段，只统计预热结束之后到程序退出的样本
    const double warmUpSeconds = 1.5;
    uint32_t reportDrawCallCount = 0;

    GpuBuffer captureBuffer = {};
    const bool captureRequested = !capturePath.empty();
    if (captureRequested) {
        createCaptureBuffer(ctx, captureBuffer);
    }
    bool captureDone = false;

    ControlServer controlServer;
    bool usedSegments = false;
    bool segmentActive = false;
    double segmentStartSeconds = 0.0;
    bool quitRequested = false;

    controlServer.onCommand = [&](const std::string& command) -> std::string {
        std::istringstream stream(command);
        std::string verb;
        stream >> verb;

        if (verb == "instances") {
            long value = 0;
            if (!(stream >> value) || value < 1 || value > static_cast<long>(instanceCapacity)) {
                return "err: instances out of range";
            }
            uiState.activeInstanceCount = static_cast<int>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "yaw" || verb == "pitch") {
            double value = 0.0;
            if (!(stream >> value)) {
                return "err: bad " + verb;
            }
            if (verb == "yaw") {
                uiState.lightYawDegrees = static_cast<float>(value);
            } else {
                uiState.lightPitchDegrees = static_cast<float>(value);
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "shadows" || verb == "pcf" || verb == "cascade-blend" || verb == "normal-lift" ||
            verb == "slope-bias" || verb == "ground-caster") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: " + verb + " takes 0 or 1";
            }
            const bool enabled = value == 1;
            if (verb == "shadows") {
                uiState.shadowsEnabled = enabled;
            } else if (verb == "pcf") {
                uiState.pcfEnabled = enabled;
            } else if (verb == "cascade-blend") {
                uiState.cascadeBlend = enabled;
            } else if (verb == "normal-lift") {
                uiState.shadowNormalLift = enabled;
            } else if (verb == "slope-bias") {
                uiState.shadowSlopeBias = enabled;
            } else {
                uiState.shadowGroundCaster = enabled;
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "cascades") {
            long value = 0;
            if (!(stream >> value) || (value != 1 && value != 2 && value != 4)) {
                return "err: cascades takes 1, 2 or 4";
            }
            uiState.cascadeCount = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "shadow-size") {
            long value = 0;
            if (!(stream >> value) || value < SHADOW_MAP_MIN_SIZE || value > SHADOW_MAP_MAX_SIZE) {
                return "err: shadow-size out of range";
            }
            uiState.shadowMapSize = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "value-mode") {
            std::string value;
            if (!(stream >> value) || (value != "depth" && value != "vsm")) {
                return "err: value-mode takes depth or vsm";
            }
            uiState.valueMode = value == "vsm" ? SHADOW_VALUE_VSM : SHADOW_VALUE_DEPTH;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "coord-mode") {
            std::string value;
            if (!(stream >> value) || (value != "fragment" && value != "vertex")) {
                return "err: coord-mode takes fragment or vertex";
            }
            uiState.coordMode = value == "vertex" ? SHADOW_COORD_VERTEX : SHADOW_COORD_FRAGMENT;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "view") {
            std::string value;
            if (!(stream >> value) ||
                (value != "final" && value != "cascades" && value != "visibility")) {
                return "err: view takes final, cascades or visibility";
            }
            uiState.viewMode = value == "cascades" ? SHADOW_VIEW_CASCADES
                              : value == "visibility" ? SHADOW_VIEW_VISIBILITY
                                                       : SHADOW_VIEW_FINAL;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "blocker-radius" || verb == "penumbra" || verb == "depth-offset") {
            double value = 0.0;
            if (!(stream >> value) || value < 0.0) {
                return "err: " + verb + " out of range";
            }
            if (verb == "blocker-radius") {
                uiState.blockerRadius = static_cast<float>(value);
            } else if (verb == "penumbra") {
                uiState.penumbraScale = static_cast<float>(value);
            } else {
                uiState.shadowDepthOffset = static_cast<float>(value);
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "begin") {
            usedSegments = true;
            segmentActive = true;
            segmentStartSeconds = nowSeconds();
            reportDrawCallCount = 0;
            resetTimingReport(timingStore);
            return "ok";
        }
        if (verb == "end") {
            if (!segmentActive) {
                return "err: no segment started";
            }
            if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
                return "err: segment had no sampled frames";
            }
            const std::string line =
                reportPrefix(uiState, reportDrawCallCount) + timingReportValueColumns(timingStore);
            if (!reportPath.empty()) {
                appendMeasurementReport(reportPath, reportHeaderColumns, line);
            }
            segmentActive = false;
            resetTimingReport(timingStore);
            return "row " + line;
        }
        if (verb == "quit") {
            quitRequested = true;
            return "ok";
        }
        return "err: unknown command";
    };

    if (controlPort != 0 && !controlServer.start(controlPort)) {
        FATAL("failed to open the TCP control server on port %u", controlPort);
    }

    if (interfaceEnabled) {
        startGpuClockMonitor(gpuClockMonitor, gpuClockLockState, startTime);
    }

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        controlServer.pump();
        if (quitRequested) {
            break;
        }

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth == 0 || framebufferHeight == 0) {
            if (autoExitSeconds > 0.0 && nowSeconds() - startTime >= autoExitSeconds) {
                break;
            }
            continue;
        }

        if (static_cast<uint32_t>(framebufferWidth) != ctx.swapchainExtent.width ||
            static_cast<uint32_t>(framebufferHeight) != ctx.swapchainExtent.height) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
        }

        if (interfaceEnabled) {
            beginUserInterfaceFrame();
            buildUserInterface(uiState, uiStatistics, timingStore, static_cast<int>(instanceCapacity),
                               gpuClockLockState, gpuClockMonitor);
        }

        const bool keyboardGoesToInterface = interfaceEnabled && userInterfaceWantsKeyboard();
        if (!keyboardGoesToInterface && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            if (interfaceEnabled) {
                endUserInterfaceFrame();
            }
            break;
        }

        const double currentTime = nowSeconds();
        const float deltaSeconds = static_cast<float>(currentTime - previousTime);
        previousTime = currentTime;

        camera.moveSpeed = uiState.cameraMoveSpeed;
        if (!keyboardGoesToInterface) {
            CameraInput cameraInput = {};
            cameraInput.turnLeft = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
            cameraInput.turnRight = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            cameraInput.turnUp = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
            cameraInput.turnDown = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
            cameraInput.moveForward = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
            cameraInput.moveBack = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
            cameraInput.moveLeft = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
            cameraInput.moveRight = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
            cameraInput.moveUp = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
            cameraInput.moveDown = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
            cameraInput.fast = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            updateCamera(camera, cameraInput, deltaSeconds);
        }

        const uint32_t activeInstanceCount = static_cast<uint32_t>(uiState.activeInstanceCount);

        // 光源方向：方位角绕 Y 轴，高度角为与水平面的夹角
        const float yaw = glm::radians(uiState.lightYawDegrees);
        const float pitch = glm::radians(uiState.lightPitchDegrees);
        const glm::vec3 lightDirection =
            glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                     -std::cos(pitch) * std::cos(yaw)));

        shadowOptions.mapSize = uiState.shadowMapSize;
        shadowOptions.cascadeCount = uiState.cascadeCount;
        shadowOptions.cascadeBlend = uiState.cascadeBlend;
        shadowOptions.normalLift = uiState.shadowNormalLift;
        shadowOptions.slopeBias = uiState.shadowSlopeBias;
        shadowOptions.depthOffset = uiState.shadowDepthOffset;
        shadowOptions.groundCaster = uiState.shadowGroundCaster;
        shadowOptions.valueMode = uiState.valueMode;
        shadowOptions.coordMode = uiState.coordMode;
        shadowOptions.blockerRadius = uiState.blockerRadius;
        shadowOptions.penumbraScale = uiState.penumbraScale;

        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

        ShadowSceneUniform sceneUniform;
        fillShadowUniform(camera, aspectRatio, lightDirection, shadowOptions, uiState, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.activeInstanceCount = activeInstanceCount;
        input.drawUserInterface = interfaceEnabled;
        input.shadowsEnabled = uiState.shadowsEnabled;
        input.pcfRadius = uiState.pcfEnabled ? 1.0f : 0.0f;
        input.shadow = shadowOptions;
        input.viewMode = uiState.viewMode;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, sceneUniform, statistics);
        if (!frameDrawn) {
            rebuildSwapchainResources(ctx, renderer, captureBuffer, captureRequested && !captureDone);
            continue;
        }
        if (shouldCaptureThisFrame) {
            captureDone = true;
        }
        ++frameCounter;

        uiStatistics.drawCallCount = statistics.drawCallCount;

        const bool includeInReport =
            usedSegments ? (segmentActive && currentTime - segmentStartSeconds >= warmUpSeconds)
                         : (currentTime - startTime >= warmUpSeconds);
        if (includeInReport) {
            reportDrawCallCount = statistics.drawCallCount;
        }

        // 按 TimingId 的顺序填满全部计时项，新增计时项时只需要在这里补一行
        double timingValues[TIMING_ID_COUNT];
        timingValues[TIMING_FRAME] = static_cast<double>(deltaSeconds) * 1000.0;
        timingValues[TIMING_CPU_RECORD_BEGIN] = statistics.cpuRecordBeginMilliseconds;
        timingValues[TIMING_CPU_RECORD_SHADOW_PASS] = statistics.cpuRecordShadowPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_MAIN_PASS] = statistics.cpuRecordMainPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("cascades %u | map %u | value %s | coords %s | draw commands %u\n",
                        uiState.cascadeCount, uiState.shadowMapSize, shadowValueModeName(uiState.valueMode),
                        uiState.coordMode == SHADOW_COORD_VERTEX ? "vertex" : "fragment",
                        uiStatistics.drawCallCount);
            for (int i = 0; i < TIMING_ID_COUNT; ++i) {
                const TimingWindow& window = timingStore.window[i];
                if (i == TIMING_FRAME) {
                    const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
                    std::printf("  %-30s %7.3f +/- %6.3f ms (%.0f FPS)\n",
                                timingStore.items[i].reportColumn, window.mean, window.standardDeviation, fps);
                } else {
                    std::printf("  %-30s %7.3f +/- %6.3f ms\n", timingStore.items[i].reportColumn, window.mean,
                                window.standardDeviation);
                }
            }
            lastPrintTime = currentTime;
        }

        if (shouldExit) {
            break;
        }
    }

    controlServer.stop();
    stopGpuClockMonitor(gpuClockMonitor);

    VK_CHECK(vkDeviceWaitIdle(ctx.device));

    if (captureRequested) {
        writeCaptureBufferToPng(ctx, captureBuffer, capturePath);
        destroyBuffer(ctx, captureBuffer);
    }

    std::printf("submitted %llu frames, last frame draw commands %u\n",
                static_cast<unsigned long long>(frameCounter), uiStatistics.drawCallCount);

    if (!reportPath.empty() && !usedSegments) {
        if (timingStore.report[TIMING_FRAME].sampleCount == 0) {
            FATAL("no frame was sampled in the measurement window, set --auto-exit longer than the warm-up time");
        }
        const std::string line = reportPrefix(uiState, reportDrawCallCount) +
                                 timingReportValueColumns(timingStore);
        appendMeasurementReport(reportPath, reportHeaderColumns, line);
    }

    destroyUserInterface(ctx, ui);
    destroyRenderer(ctx, renderer);
    destroySwapchain(ctx);
    destroyVulkanContext(ctx);

    glfwDestroyWindow(window);
    glfwTerminate();

    releaseGpuClockLockOnExit(gpuClockLockState);

    restoreConsoleEncoding();
    return EXIT_SUCCESS;
}
