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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 相机放在地面正上方，俯角 20 度朝远处看。地面的两层在画面下半部分还分得清，
// 越靠近地平线深度值越挤，Z-fighting 从画面中部开始向上加重
static const float CAMERA_HEIGHT = 66.0f;
static const float CAMERA_PITCH_RADIANS = -0.349066f;

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS = "objects,draw_commands";

static std::string reverseZReportPrefix(uint32_t objects, uint32_t drawCommands)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "%u,%u", objects, drawCommands);
    return std::string(prefix);
}

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建。
// 抓帧缓冲的大小也由交换链尺寸决定，抓帧还没发生时一并重建
static void rebuildSwapchainResources(VulkanContext& ctx, ReverseZRenderer& renderer,
                                      GpuBuffer& captureBuffer, bool capturePending)
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

int main(int argc, char** argv)
{
    configureConsoleEncoding();

    uint32_t initialObjectCount = 12;
    double autoExitSeconds = 0.0;
    std::string capturePath;
    std::string reportPath;
    bool interfaceEnabled = true;
    uint16_t controlPort = 0;
    uint32_t requestedCoreClockMHz = 0;
    uint32_t requestedMemoryClockMHz = 0;
    bool reverseZ = false;
    float nearPlane = 0.1f;
    float farPlane = 20000.0f;
    float groundOffset = 0.01f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--objects") == 0 && i + 1 < argc) {
            initialObjectCount = static_cast<uint32_t>(std::atoi(argv[i + 1]));
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
        } else if (std::strcmp(argv[i], "--reverse-z") == 0) {
            reverseZ = true;
        } else if (std::strcmp(argv[i], "--no-reverse-z") == 0) {
            reverseZ = false;
        } else if (std::strcmp(argv[i], "--near") == 0 && i + 1 < argc) {
            nearPlane = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--far") == 0 && i + 1 < argc) {
            farPlane = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--ground-offset") == 0 && i + 1 < argc) {
            groundOffset = static_cast<float>(std::atof(argv[i + 1]));
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

    if (nearPlane < NEAR_PLANE_MIN || nearPlane > NEAR_PLANE_MAX) {
        FATAL("--near must be between %f and %f", static_cast<double>(NEAR_PLANE_MIN),
              static_cast<double>(NEAR_PLANE_MAX));
    }
    if (farPlane < FAR_PLANE_MIN || farPlane > FAR_PLANE_MAX) {
        FATAL("--far must be between %f and %f", static_cast<double>(FAR_PLANE_MIN),
              static_cast<double>(FAR_PLANE_MAX));
    }
    if (nearPlane >= farPlane) {
        FATAL("--near must be smaller than --far");
    }
    if (groundOffset < GROUND_OFFSET_MIN || groundOffset > GROUND_OFFSET_MAX) {
        FATAL("--ground-offset must be between %f and %f", static_cast<double>(GROUND_OFFSET_MIN),
              static_cast<double>(GROUND_OFFSET_MAX));
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

    const uint32_t objectCapacity = std::max(64u, initialObjectCount);

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit failed");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("no Vulkan loader is available in this environment");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Reverse-Z Demo", nullptr, nullptr);
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
    buildGroundStripMesh(groundMesh);

    const float objectCenterHeight = objectCenterHeightForMesh(objectMesh);

    // 实例缓冲的前两项是两层地面，其后是地面上的物体
    std::vector<InstanceData> instances;
    buildGroundInstances(instances);
    const uint32_t objectInstanceOffset = static_cast<uint32_t>(instances.size());

    std::vector<InstanceData> objectInstances;
    buildObjectInstances(objectCapacity, objectCenterHeight, objectInstances);
    instances.insert(instances.end(), objectInstances.begin(), objectInstances.end());

    ReverseZRenderer renderer = {};
    createRenderer(ctx, renderer, objectMesh, groundMesh, instances, objectInstanceOffset, reverseZ);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.renderPass, caseTexts, caseTextCount, ui);

    Camera camera = {};
    camera.position = glm::vec3(0.0f, CAMERA_HEIGHT, 0.0f);
    camera.yaw = 0.0f;
    camera.pitch = CAMERA_PITCH_RADIANS;
    camera.verticalFieldOfView = glm::radians(60.0f);
    camera.nearPlane = nearPlane;
    camera.farPlane = farPlane;
    camera.moveSpeed = 60.0f;

    UiState uiState = {};
    uiState.activeObjectCount = static_cast<int>(initialObjectCount);
    uiState.cameraMoveSpeed = camera.moveSpeed;
    uiState.reverseZ = reverseZ;
    uiState.nearPlane = nearPlane;
    uiState.farPlane = farPlane;
    uiState.groundOffset = groundOffset;

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

        if (verb == "objects") {
            long value = 0;
            if (!(stream >> value) || value < 0 || value > static_cast<long>(objectCapacity)) {
                return "err: objects out of range";
            }
            uiState.activeObjectCount = static_cast<int>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "reverse-z") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: reverse-z takes 0 or 1";
            }
            uiState.reverseZ = value == 1;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "near") {
            double value = 0.0;
            if (!(stream >> value) || value < NEAR_PLANE_MIN || value > NEAR_PLANE_MAX) {
                return "err: near out of range";
            }
            uiState.nearPlane = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "far") {
            double value = 0.0;
            if (!(stream >> value) || value < FAR_PLANE_MIN || value > FAR_PLANE_MAX) {
                return "err: far out of range";
            }
            uiState.farPlane = static_cast<float>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "ground-offset") {
            double value = 0.0;
            if (!(stream >> value) || value < GROUND_OFFSET_MIN || value > GROUND_OFFSET_MAX) {
                return "err: ground-offset out of range";
            }
            uiState.groundOffset = static_cast<float>(value);
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
                reverseZReportPrefix(static_cast<uint32_t>(uiState.activeObjectCount), reportDrawCallCount) +
                timingReportValueColumns(timingStore);
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
            buildUserInterface(uiState, uiStatistics, timingStore, static_cast<int>(objectCapacity),
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
        camera.nearPlane = uiState.nearPlane;
        camera.farPlane = uiState.farPlane;
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

        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

        SceneUniform sceneUniform;
        fillSceneUniform(camera, aspectRatio, uiState.reverseZ, uiState.groundOffset, sceneUniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.reverseZ = uiState.reverseZ;
        input.activeObjectCount = static_cast<uint32_t>(uiState.activeObjectCount);
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
        timingValues[TIMING_CPU_RECORD_MAIN_PASS] = statistics.cpuRecordMainPassMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("objects %d | draw commands %u | depth %s | near %.3f far %.0f ground offset %.3f\n",
                        uiState.activeObjectCount, uiStatistics.drawCallCount,
                        uiState.reverseZ ? "reverse-z" : "standard", static_cast<double>(uiState.nearPlane),
                        static_cast<double>(uiState.farPlane), static_cast<double>(uiState.groundOffset));
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
        const std::string line =
            reverseZReportPrefix(static_cast<uint32_t>(uiState.activeObjectCount), reportDrawCallCount) +
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
