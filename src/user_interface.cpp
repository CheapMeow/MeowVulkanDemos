#include "user_interface.h"

#include "vk_check.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>

// 界面上的全部文本。字体字形范围、启动校验与界面绘制都以这份定义为唯一来源，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan Indirect Draw 对比";

static const char* const TEXT_SECTION_PATH = "绘制路径";
static const char* const TEXT_PATH_TRADITIONAL = "逐实例 drawIndexed（主机剔除，每个可见实例一条命令）";
static const char* const TEXT_PATH_INSTANCED = "实例化 drawIndexed（主机剔除，一条命令）";
static const char* const TEXT_PATH_INDIRECT = "indirect（计算着色器剔除，一条命令）";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_LIGHT_COUNT = "光源数量";
static const char* const TEXT_FAR_PLANE = "远裁剪面";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_TIMING = "本帧耗时";
static const char* const TEXT_TIMING_HINT = "以下每一项均为最近 100 帧滑动窗口内的均值与标准差，下方曲线画出最近 10 秒，横轴为启动以来的秒数，纵轴单位为毫秒，上下限取这段时间内帧的最小值与最大值再各留一成余量";

// 曲线上画出最近多少秒的数据，横轴范围与纵轴上下限都取自这一批数据
static const float TIMING_PLOT_VISIBLE_SECONDS = 10.0f;

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_TOTAL_INSTANCES = "实例总数 %d";
static const char* const TEXT_VISIBLE_INSTANCES = "可见实例 %u";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_SWITCH = "空格键依次切换三条绘制路径，效果与上面的单选按钮相同";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_SECTION_GPU_LOCK = "GPU 锁频";
static const char* const TEXT_GPU_NOT_DETECTED = "未探测到支持锁频的 NVIDIA 显卡：%s";
static const char* const TEXT_GPU_DEVICE_NAME = "显卡 %s（设备 #%u）";
static const char* const TEXT_GPU_QUERY_BUTTON = "查询当前频率";
static const char* const TEXT_GPU_CURRENT_CLOCKS = "上次查询：核心 %u MHz，显存 %u MHz";
static const char* const TEXT_GPU_CLOCKS_NOT_QUERIED =
    "尚未查询当前频率。查询要启动 nvidia-smi 并阻塞主线程，因此不做定时轮询，只在按下按钮时查一次";
static const char* const TEXT_GPU_QUERY_FAILED = "查询频率失败：%s";
static const char* const TEXT_GPU_CORE_CLOCK_PLOT = "核心频率";
static const char* const TEXT_GPU_MEMORY_CLOCK_PLOT = "显存频率";
static const char* const TEXT_GPU_CLOCK_PLOT_HINT =
    "下面两张图每 1 秒在后台线程采样一次，采样不占用主线程，等待 nvidia-smi 的时间不会进到帧时间里。"
    "横轴与耗时曲线同为启动以来的秒数，同样只画最近十秒，纵轴单位为 MHz";
static const char* const TEXT_GPU_TARGET_CORE = "目标核心频率";
static const char* const TEXT_GPU_TARGET_MEMORY = "目标显存频率";
static const char* const TEXT_GPU_LOCK_BUTTON = "锁频";
static const char* const TEXT_GPU_UNLOCK_BUTTON = "解锁";
static const char* const TEXT_GPU_LOCKED_STATUS = "已锁定：核心 %u MHz，显存 %u MHz";
static const char* const TEXT_GPU_NOT_LOCKED_STATUS = "未锁定，自动调频";
static const char* const TEXT_GPU_LOCK_FAILED = "锁频失败，消费级显卡常见拒绝 -lgc/-lmc，或者需要以管理员身份运行本程序：%s";
static const char* const TEXT_GPU_UNLOCK_FAILED = "解锁失败：%s";

static const char* const TEXT_EXPLANATION =
    "三条路径共用同一份着色器与同一套剔除判据，画面完全一致。"
    "把实例数量或者远裁剪面调大，观察主机剔除与主机记录命令这两项的变化，"
    "设备时间在三条路径上保持一致。";

static const char* const ALL_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,         TEXT_SECTION_PATH,        TEXT_PATH_TRADITIONAL,      TEXT_PATH_INSTANCED,
    TEXT_PATH_INDIRECT,       TEXT_SECTION_SCENE,       TEXT_INSTANCE_COUNT,        TEXT_LIGHT_COUNT,
    TEXT_FAR_PLANE,           TEXT_MOVE_SPEED,          TEXT_SECTION_TIMING,        TEXT_TIMING_HINT,
    TEXT_SECTION_WORKLOAD,    TEXT_TOTAL_INSTANCES,
    TEXT_VISIBLE_INSTANCES,   TEXT_DRAW_COMMANDS,       TEXT_SECTION_GUIDE,         TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,          TEXT_GUIDE_SWITCH,        TEXT_GUIDE_QUIT,            TEXT_GUIDE_DRAG,
    TEXT_EXPLANATION,
    TEXT_SECTION_GPU_LOCK,    TEXT_GPU_NOT_DETECTED,    TEXT_GPU_DEVICE_NAME,       TEXT_GPU_CURRENT_CLOCKS,
    TEXT_GPU_TARGET_CORE,     TEXT_GPU_TARGET_MEMORY,   TEXT_GPU_LOCK_BUTTON,       TEXT_GPU_UNLOCK_BUTTON,
    TEXT_GPU_LOCKED_STATUS,   TEXT_GPU_NOT_LOCKED_STATUS, TEXT_GPU_LOCK_FAILED,     TEXT_GPU_UNLOCK_FAILED,
    TEXT_GPU_QUERY_BUTTON,    TEXT_GPU_CLOCKS_NOT_QUERIED, TEXT_GPU_QUERY_FAILED,
    TEXT_GPU_CORE_CLOCK_PLOT, TEXT_GPU_MEMORY_CLOCK_PLOT,  TEXT_GPU_CLOCK_PLOT_HINT,
    timingDisplayName(TIMING_FRAME),
    timingDisplayName(TIMING_CPU_CULL),
    timingDisplayName(TIMING_CPU_RECORD_BEGIN),
    timingDisplayName(TIMING_CPU_RECORD_CULL_DISPATCH),
    timingDisplayName(TIMING_CPU_RECORD_GBUFFER_PASS),
    timingDisplayName(TIMING_CPU_RECORD_LIGHTING_PASS),
    timingDisplayName(TIMING_CPU_RECORD_UI),
    timingDisplayName(TIMING_CPU_RECORD_CAPTURE),
    timingDisplayName(TIMING_CPU_RECORD_SUBMIT),
    timingDisplayName(TIMING_GPU_TOTAL),
};

enum { INTERFACE_TEXT_COUNT = sizeof(ALL_INTERFACE_TEXTS) / sizeof(ALL_INTERFACE_TEXTS[0]) };

// 字形范围需要在字体图集构建期间保持有效
static ImVector<ImWchar> gGlyphRanges;

static void checkImGuiResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        FATAL("imgui Vulkan call failed");
    }
}

// 界面文本里超出基本拉丁字母范围的字符统计
struct GlyphUsage {
    int ideographCount;    // 汉字
    int punctuationCount;  // 中日韩标点与全角符号
};

// 缺少字形的字符会被替换成问号，直接在启动时判定为失败
static GlyphUsage verifyGlyphsPresent(ImFont* font)
{
    GlyphUsage usage = {};

    for (int i = 0; i < INTERFACE_TEXT_COUNT; ++i) {
        const char* cursor = ALL_INTERFACE_TEXTS[i];
        while (*cursor != '\0') {
            unsigned int codepoint = 0;
            const int consumedBytes = ImTextCharFromUtf8(&codepoint, cursor, nullptr);
            if (consumedBytes == 0) {
                FATAL("cannot decode the character encoding of the interface text");
            }
            cursor += consumedBytes;

            if (font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) == nullptr) {
                FATAL("font lacks a glyph required by the interface text, code point U+%04X", codepoint);
            }

            if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
                ++usage.ideographCount;
            } else if ((codepoint >= 0x3000 && codepoint <= 0x303F) ||
                       (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) {
                ++usage.punctuationCount;
            }
        }
    }

    return usage;
}

void createUserInterface(const VulkanContext& ctx, const Renderer& renderer, UserInterface& ui)
{
    // 界面只需要采样字体图集，一个组合图像采样器就够
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 8;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &ui.descriptorPool));

    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 默认字体没有汉字，使用系统自带的微软雅黑，字形范围由界面文本本身决定
    ImFontGlyphRangesBuilder rangesBuilder;
    rangesBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (int i = 0; i < INTERFACE_TEXT_COUNT; ++i) {
        rangesBuilder.AddText(ALL_INTERFACE_TEXTS[i]);
    }
    rangesBuilder.BuildRanges(&gGlyphRanges);

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    ImFont* font =
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18.0f, &fontConfig, gGlyphRanges.Data);
    if (font == nullptr) {
        FATAL("failed to load font C:/Windows/Fonts/msyh.ttc");
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);

    if (!ImGui_ImplGlfw_InitForVulkan(ctx.window, true)) {
        FATAL("failed to initialize the imgui GLFW backend");
    }

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = VK_API_VERSION_1_2;
    initInfo.Instance = ctx.instance;
    initInfo.PhysicalDevice = ctx.physicalDevice;
    initInfo.Device = ctx.device;
    initInfo.QueueFamily = ctx.queueFamilyIndex;
    initInfo.Queue = ctx.queue;
    initInfo.DescriptorPool = ui.descriptorPool;
    initInfo.RenderPass = renderer.lightingRenderPass;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = ctx.swapchainImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Subpass = 0;
    initInfo.CheckVkResultFn = checkImGuiResult;
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        FATAL("failed to initialize the imgui Vulkan backend");
    }
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        FATAL("failed to create the imgui font texture");
    }

    const GlyphUsage usage = verifyGlyphsPresent(font);
    std::printf("interface font msyh.ttc: %d glyphs loaded, interface text uses %d ideographs and %d full-width "
                "punctuation marks, all present\n",
                font->Glyphs.Size, usage.ideographCount, usage.punctuationCount);
}

void destroyUserInterface(const VulkanContext& ctx, UserInterface& ui)
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(ctx.device, ui.descriptorPool, nullptr);
    ui.descriptorPool = VK_NULL_HANDLE;
}

void beginUserInterfaceFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

// 画一张只覆盖最近十秒的曲线，横轴自动缩放到这段时间，纵轴上下限取同一批数据的最小值与最大值
// 再各向外留一成余量。曲线图高度要给够：坐标轴刻度文字加上四周留白会占掉近七十像素，高度只有
// 七十像素时真正画曲线的区域只剩十几像素，纵轴范围怎么调都是被压平的一条线。坐标轴标题省掉，
// 单位写进上方的说明文字，坐标轴上只留刻度数值
static void plotRecentSeries(const char* name, const std::vector<float>& times,
                             const std::vector<float>& values)
{
    ImGui::PushID(name);
    char plotId[96];
    std::snprintf(plotId, sizeof(plotId), "%s##plot", name);
    if (ImPlot::BeginPlot(plotId, ImVec2(-1.0f, 150.0f), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);

        if (!times.empty()) {
            const float windowStart = std::max(0.0f, times.back() - TIMING_PLOT_VISIBLE_SECONDS);
            const int startIndex = static_cast<int>(std::lower_bound(times.begin(), times.end(), windowStart) -
                                                    times.begin());
            const int visibleCount = static_cast<int>(times.size()) - startIndex;
            const float* valueBegin = values.data() + startIndex;
            const auto bounds = std::minmax_element(valueBegin, valueBegin + visibleCount);
            const double minimum = static_cast<double>(*bounds.first);
            const double maximum = static_cast<double>(*bounds.second);
            // 长时间不变的量（例如锁频成功之后的频率）最大值减最小值是零，给一个最小跨度，
            // 免得上下限相等画不出网格。跨度下限同时取最大值的一个百分点，百万赫兹量级与毫秒
            // 量级都能得到合适的范围
            const double range = std::max(maximum - minimum, std::max(std::abs(maximum) * 0.01, 0.1));
            const double margin = range * 0.1;
            ImPlot::SetupAxisLimits(ImAxis_Y1, std::max(0.0, minimum - margin), maximum + margin,
                                    ImPlotCond_Always);
            ImPlot::PlotLine(name, times.data() + startIndex, valueBegin, visibleCount);
        }
        ImPlot::EndPlot();
    }
    ImGui::PopID();
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxInstanceCount, int maxLightCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 860.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_PATH);
    int selectedPath = static_cast<int>(state.drawPath);
    ImGui::RadioButton(TEXT_PATH_TRADITIONAL, &selectedPath, DRAW_PATH_TRADITIONAL);
    ImGui::RadioButton(TEXT_PATH_INSTANCED, &selectedPath, DRAW_PATH_INSTANCED);
    ImGui::RadioButton(TEXT_PATH_INDIRECT, &selectedPath, DRAW_PATH_INDIRECT);
    state.drawPath = static_cast<DrawPath>(selectedPath);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_INSTANCE_COUNT, &state.activeInstanceCount, 1, maxInstanceCount, "%d",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt(TEXT_LIGHT_COUNT, &state.activeLightCount, 1, maxLightCount);
    ImGui::SliderFloat(TEXT_FAR_PLANE, &state.farPlane, 40.0f, 900.0f, "%.0f");
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 5.0f, 400.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_GPU_LOCK);
    if (!gpuClockLockState.detected) {
        ImGui::TextWrapped(TEXT_GPU_NOT_DETECTED, gpuClockLockState.lastActionDetail.c_str());
    } else {
        ImGui::Text(TEXT_GPU_DEVICE_NAME, gpuClockLockState.gpuName.c_str(), gpuClockLockState.gpuIndex);

        if (ImGui::Button(TEXT_GPU_QUERY_BUTTON)) {
            queryLiveGpuClocks(gpuClockLockState);
        }
        if (gpuClockLockState.clocksQueried) {
            ImGui::Text(TEXT_GPU_CURRENT_CLOCKS, gpuClockLockState.currentCoreClockMHz,
                        gpuClockLockState.currentMemoryClockMHz);
        } else if (gpuClockLockState.lastAction == GpuClockLockAction::kQueryFailed) {
            ImGui::TextWrapped(TEXT_GPU_QUERY_FAILED, gpuClockLockState.lastActionDetail.c_str());
        } else {
            ImGui::TextWrapped(TEXT_GPU_CLOCKS_NOT_QUERIED);
        }

        char comboLabel[32];

        ImGui::BeginDisabled(gpuClockLockState.locked);

        std::snprintf(comboLabel, sizeof(comboLabel), "%u MHz",
                      gpuClockLockState.supportedCoreClocksMHz[gpuClockLockState.selectedCoreClockIndex]);
        if (ImGui::BeginCombo(TEXT_GPU_TARGET_CORE, comboLabel)) {
            for (int i = 0; i < static_cast<int>(gpuClockLockState.supportedCoreClocksMHz.size()); ++i) {
                const bool selected = i == gpuClockLockState.selectedCoreClockIndex;
                char itemLabel[32];
                std::snprintf(itemLabel, sizeof(itemLabel), "%u MHz", gpuClockLockState.supportedCoreClocksMHz[i]);
                if (ImGui::Selectable(itemLabel, selected)) {
                    gpuClockLockState.selectedCoreClockIndex = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        std::snprintf(comboLabel, sizeof(comboLabel), "%u MHz",
                      gpuClockLockState.supportedMemoryClocksMHz[gpuClockLockState.selectedMemoryClockIndex]);
        if (ImGui::BeginCombo(TEXT_GPU_TARGET_MEMORY, comboLabel)) {
            for (int i = 0; i < static_cast<int>(gpuClockLockState.supportedMemoryClocksMHz.size()); ++i) {
                const bool selected = i == gpuClockLockState.selectedMemoryClockIndex;
                char itemLabel[32];
                std::snprintf(itemLabel, sizeof(itemLabel), "%u MHz",
                             gpuClockLockState.supportedMemoryClocksMHz[i]);
                if (ImGui::Selectable(itemLabel, selected)) {
                    gpuClockLockState.selectedMemoryClockIndex = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button(TEXT_GPU_LOCK_BUTTON)) {
            requestLockGpuClocks(gpuClockLockState);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!gpuClockLockState.locked);
        if (ImGui::Button(TEXT_GPU_UNLOCK_BUTTON)) {
            requestUnlockGpuClocks(gpuClockLockState);
        }
        ImGui::EndDisabled();

        if (gpuClockLockState.lastAction == GpuClockLockAction::kLockFailed) {
            ImGui::TextWrapped(TEXT_GPU_LOCK_FAILED, gpuClockLockState.lastActionDetail.c_str());
        } else if (gpuClockLockState.lastAction == GpuClockLockAction::kUnlockFailed) {
            ImGui::TextWrapped(TEXT_GPU_UNLOCK_FAILED, gpuClockLockState.lastActionDetail.c_str());
        } else if (gpuClockLockState.locked) {
            ImGui::Text(TEXT_GPU_LOCKED_STATUS, gpuClockLockState.lockedCoreClockMHz,
                        gpuClockLockState.lockedMemoryClockMHz);
        } else {
            ImGui::TextUnformatted(TEXT_GPU_NOT_LOCKED_STATUS);
        }

        ImGui::TextWrapped(TEXT_GPU_CLOCK_PLOT_HINT);
        std::vector<float> clockTimeSeconds;
        std::vector<float> coreClockMHz;
        std::vector<float> memoryClockMHz;
        copyGpuClockSamples(gpuClockMonitor, clockTimeSeconds, coreClockMHz, memoryClockMHz);
        plotRecentSeries(TEXT_GPU_CORE_CLOCK_PLOT, clockTimeSeconds, coreClockMHz);
        plotRecentSeries(TEXT_GPU_MEMORY_CLOCK_PLOT, clockTimeSeconds, memoryClockMHz);
    }

    ImGui::SeparatorText(TEXT_SECTION_TIMING);
    ImGui::TextWrapped("%s", TEXT_TIMING_HINT);
    ImGui::BeginChild("TimingScrollRegion", ImVec2(0.0f, 420.0f), ImGuiChildFlags_Border);
    for (int i = 0; i < TIMING_ID_COUNT; ++i) {
        const TimingId id = static_cast<TimingId>(i);
        const TimingWindow& window = timing.window[id];
        const char* name = timingDisplayName(id);

        if (id == TIMING_FRAME) {
            const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
            ImGui::Text("%s   %7.3f ± %6.3f ms   (%.0f FPS)", name, window.mean, window.standardDeviation, fps);
        } else {
            ImGui::Text("%s   %7.3f ± %6.3f ms", name, window.mean, window.standardDeviation);
        }

        plotRecentSeries(name, timing.historyTimeSeconds, timing.historyValues[id]);
    }
    ImGui::EndChild();

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_TOTAL_INSTANCES, state.activeInstanceCount);
    ImGui::Text(TEXT_VISIBLE_INSTANCES, statistics.visibleInstanceCount);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_SWITCH);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}

void endUserInterfaceFrame()
{
    ImGui::Render();
}

bool userInterfaceWantsKeyboard()
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

void recordUserInterfaceCommands(VkCommandBuffer commandBuffer)
{
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}
