#include "case_ui.h"
#include "console.h"
#include "control_server.h"
#include "frame_capture.h"
#include "gpu_clock_lock.h"
#include "renderer.h"
#include "scene.h"
#include "scene_setup.h"
#include "timing.h"
#include "timing_items.h"
#include "user_interface.h"
#include "vk_check.h"
#include "vk_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// 报告里 case 自己的前几列
static const char* const REPORT_HEADER_COLUMNS =
    "lighting,distribution,geometry,fresnel,multiscatter,roughness,metallic,base_color,draw_commands";

static bool parseLighting(const std::string& value, uint32_t& lighting)
{
    if (value == "directional") {
        lighting = BRDF_LIGHTING_DIRECTIONAL;
    } else if (value == "furnace") {
        lighting = BRDF_LIGHTING_FURNACE;
    } else if (value == "table") {
        lighting = BRDF_LIGHTING_TABLE;
    } else {
        return false;
    }
    return true;
}

static bool parseDistribution(const std::string& value, uint32_t& distribution)
{
    if (value == "ggx") {
        distribution = BRDF_DISTRIBUTION_GGX;
    } else if (value == "beckmann") {
        distribution = BRDF_DISTRIBUTION_BECKMANN;
    } else if (value == "blinn_phong") {
        distribution = BRDF_DISTRIBUTION_BLINN_PHONG;
    } else {
        return false;
    }
    return true;
}

static bool parseGeometry(const std::string& value, uint32_t& geometry)
{
    if (value == "smith") {
        geometry = BRDF_GEOMETRY_SMITH;
    } else if (value == "schlick") {
        geometry = BRDF_GEOMETRY_SCHLICK;
    } else if (value == "none") {
        geometry = BRDF_GEOMETRY_NONE;
    } else {
        return false;
    }
    return true;
}

static bool parseFresnel(const std::string& value, uint32_t& fresnel)
{
    if (value == "schlick") {
        fresnel = BRDF_FRESNEL_SCHLICK;
    } else if (value == "constant") {
        fresnel = BRDF_FRESNEL_CONSTANT;
    } else {
        return false;
    }
    return true;
}

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

// 交换链以及所有跟它的尺寸、图像数量绑定的资源都要在窗口尺寸变化后重建
static void rebuildSwapchainResources(VulkanContext& ctx, BrdfRenderer& renderer, GpuBuffer& captureBuffer,
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

int main(int argc, char** argv)
{
    configureConsoleEncoding();

    double autoExitSeconds = 0.0;
    std::string capturePath;
    std::string reportPath;
    bool interfaceEnabled = true;
    uint16_t controlPort = 0;
    uint32_t requestedCoreClockMHz = 0;
    uint32_t requestedMemoryClockMHz = 0;

    UiState uiState = {};
    uiState.cameraMoveSpeed = 3.0f;
    uiState.lighting = BRDF_LIGHTING_DIRECTIONAL;
    uiState.distribution = BRDF_DISTRIBUTION_GGX;
    uiState.geometry = BRDF_GEOMETRY_SMITH;
    uiState.fresnel = BRDF_FRESNEL_SCHLICK;
    uiState.multiScattering = true;
    uiState.roughness = 0.4f;
    uiState.metallic = 0.0f;
    uiState.baseColor = 0.7f;
    uiState.lightIntensity = 3.0f;
    uiState.environmentRadiance = 0.5f;
    uiState.furnaceSamples = 128;
    uiState.exposure = 1.0f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--lighting") == 0 && i + 1 < argc) {
            if (!parseLighting(argv[i + 1], uiState.lighting)) {
                FATAL("--lighting takes directional, furnace or table");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--distribution") == 0 && i + 1 < argc) {
            if (!parseDistribution(argv[i + 1], uiState.distribution)) {
                FATAL("--distribution takes ggx, beckmann or blinn_phong");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--geometry") == 0 && i + 1 < argc) {
            if (!parseGeometry(argv[i + 1], uiState.geometry)) {
                FATAL("--geometry takes smith, schlick or none");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--fresnel") == 0 && i + 1 < argc) {
            if (!parseFresnel(argv[i + 1], uiState.fresnel)) {
                FATAL("--fresnel takes schlick or constant");
            }
            ++i;
        } else if (std::strcmp(argv[i], "--no-multiscatter") == 0) {
            uiState.multiScattering = false;
        } else if (std::strcmp(argv[i], "--roughness") == 0 && i + 1 < argc) {
            uiState.roughness = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--metallic") == 0 && i + 1 < argc) {
            uiState.metallic = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--base-color") == 0 && i + 1 < argc) {
            uiState.baseColor = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--light") == 0 && i + 1 < argc) {
            uiState.lightIntensity = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--environment") == 0 && i + 1 < argc) {
            uiState.environmentRadiance = static_cast<float>(std::atof(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            uiState.furnaceSamples = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            ++i;
        } else if (std::strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            uiState.exposure = static_cast<float>(std::atof(argv[i + 1]));
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

    if (glfwInit() != GLFW_TRUE) {
        FATAL("glfwInit failed");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        FATAL("no Vulkan loader is available in this environment");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Vulkan Microfacet BRDF Demo", nullptr, nullptr);
    if (window == nullptr) {
        FATAL("failed to create window");
    }

    VulkanContext ctx = {};
    createVulkanContext(ctx, true);
    createWindowSurface(ctx, window);
    createGraphicsDevice(ctx);
    createSwapchain(ctx);

    BrdfRenderer renderer = {};
    createRenderer(ctx, renderer);

    UserInterface ui = {};
    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    createUserInterface(ctx, renderer.renderPass, caseTexts, caseTextCount, ui);

    Camera camera = {};
    camera.position = glm::vec3(0.0f, 0.0f, 3.2f);
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    camera.verticalFieldOfView = glm::radians(50.0f);
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.0f;
    camera.moveSpeed = uiState.cameraMoveSpeed;

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

        if (verb == "lighting") {
            std::string value;
            if (!(stream >> value) || !parseLighting(value, uiState.lighting)) {
                return "err: lighting takes directional, furnace or table";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "distribution") {
            std::string value;
            if (!(stream >> value) || !parseDistribution(value, uiState.distribution)) {
                return "err: distribution takes ggx, beckmann or blinn_phong";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "geometry") {
            std::string value;
            if (!(stream >> value) || !parseGeometry(value, uiState.geometry)) {
                return "err: geometry takes smith, schlick or none";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "fresnel") {
            std::string value;
            if (!(stream >> value) || !parseFresnel(value, uiState.fresnel)) {
                return "err: fresnel takes schlick or constant";
            }
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "multiscatter") {
            long value = 0;
            if (!(stream >> value) || (value != 0 && value != 1)) {
                return "err: multiscatter takes 0 or 1";
            }
            uiState.multiScattering = value == 1;
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "samples") {
            long value = 0;
            if (!(stream >> value) || value < 16 || value > 256) {
                return "err: samples takes an integer between 16 and 256";
            }
            uiState.furnaceSamples = static_cast<uint32_t>(value);
            resetTimingWindows(timingStore);
            return "ok";
        }
        if (verb == "roughness" || verb == "metallic" || verb == "base-color" || verb == "light" ||
            verb == "environment" || verb == "exposure") {
            float value = 0.0f;
            if (!(stream >> value)) {
                return "err: " + verb + " takes a number";
            }
            if (verb == "roughness") {
                uiState.roughness = value;
            } else if (verb == "metallic") {
                uiState.metallic = value;
            } else if (verb == "base-color") {
                uiState.baseColor = value;
            } else if (verb == "light") {
                uiState.lightIntensity = value;
            } else if (verb == "environment") {
                uiState.environmentRadiance = value;
            } else {
                uiState.exposure = value;
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
            const std::string line = reportPrefix(uiState, reportDrawCallCount) +
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
            buildUserInterface(uiState, uiStatistics, timingStore, gpuClockLockState, gpuClockMonitor);
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

        const float aspectRatio = static_cast<float>(ctx.swapchainExtent.width) /
                                  static_cast<float>(ctx.swapchainExtent.height);

        BrdfUniform uniform;
        fillUniform(camera, aspectRatio, uiState, renderer, uniform);

        const bool shouldExit = autoExitSeconds > 0.0 && currentTime - startTime >= autoExitSeconds;
        const bool shouldCaptureThisFrame = captureRequested && !captureDone && shouldExit;

        if (interfaceEnabled) {
            endUserInterfaceFrame();
        }

        FrameInput input = {};
        input.drawUserInterface = interfaceEnabled;
        input.distribution = uiState.distribution;
        input.captureBuffer = shouldCaptureThisFrame ? &captureBuffer : nullptr;

        FrameStatistics statistics = {};
        const bool frameDrawn = drawFrame(ctx, renderer, frameCounter, input, uniform, statistics);
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
        timingValues[TIMING_CPU_RECORD_DRAW] = statistics.cpuRecordDrawMilliseconds;
        timingValues[TIMING_CPU_RECORD_UI] = statistics.cpuRecordUiMilliseconds;
        timingValues[TIMING_CPU_RECORD_CAPTURE] = statistics.cpuRecordCaptureMilliseconds;
        timingValues[TIMING_CPU_RECORD_SUBMIT] = statistics.cpuRecordSubmitMilliseconds;
        timingValues[TIMING_GPU_TOTAL] = statistics.gpuMilliseconds;
        recordFrameTimingSamples(timingStore, currentTime, includeInReport, timingValues);

        if (currentTime - lastPrintTime >= 0.5) {
            std::printf("lighting %s | distribution %s | geometry %s | rough %.3f | metallic %.2f | "
                        "multiscatter %d | draw commands %u\n",
                        brdfLightingName(uiState.lighting), brdfDistributionName(uiState.distribution),
                        brdfGeometryName(uiState.geometry), uiState.roughness, uiState.metallic,
                        uiState.multiScattering ? 1 : 0, uiStatistics.drawCallCount);
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
