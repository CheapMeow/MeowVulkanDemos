#include "user_interface.h"

#include "asset_file.h"
#include "vk_check.h"

#include <imgui.h>
#ifndef __ANDROID__
#include <imgui_impl_glfw.h>
#else
#include <imgui_impl_android.h>
#endif
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>
#include <vector>

// 公共面板上的全部文本。所有这些字符串都会进入字形范围，改动一处即生效
static const char* const TEXT_SECTION_TIMING = "本帧耗时";
static const char* const TEXT_TIMING_HINT = "以下每一项均为最近 100 帧滑动窗口内的均值与标准差，下方曲线画出最近 10 秒，横轴为启动以来的秒数，纵轴单位为毫秒，上下限取这段时间内帧的最小值与最大值再各留一成余量";

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

static const char* const COMMON_INTERFACE_TEXTS[] = {
    TEXT_SECTION_TIMING,
    TEXT_TIMING_HINT,
    TEXT_SECTION_GPU_LOCK,
    TEXT_GPU_NOT_DETECTED,
    TEXT_GPU_DEVICE_NAME,
    TEXT_GPU_QUERY_BUTTON,
    TEXT_GPU_CURRENT_CLOCKS,
    TEXT_GPU_CLOCKS_NOT_QUERIED,
    TEXT_GPU_QUERY_FAILED,
    TEXT_GPU_CORE_CLOCK_PLOT,
    TEXT_GPU_MEMORY_CLOCK_PLOT,
    TEXT_GPU_CLOCK_PLOT_HINT,
    TEXT_GPU_TARGET_CORE,
    TEXT_GPU_TARGET_MEMORY,
    TEXT_GPU_LOCK_BUTTON,
    TEXT_GPU_UNLOCK_BUTTON,
    TEXT_GPU_LOCKED_STATUS,
    TEXT_GPU_NOT_LOCKED_STATUS,
    TEXT_GPU_LOCK_FAILED,
    TEXT_GPU_UNLOCK_FAILED,
};

enum { COMMON_INTERFACE_TEXT_COUNT = sizeof(COMMON_INTERFACE_TEXTS) / sizeof(COMMON_INTERFACE_TEXTS[0]) };

// 曲线上画出最近多少秒的数据，横轴范围与纵轴上下限都取自这一批数据
static const float TIMING_PLOT_VISIBLE_SECONDS = 10.0f;

// 字形范围需要在字体图集构建期间保持有效
static ImVector<ImWchar> gGlyphRanges;
static std::vector<const char*> gAllInterfaceTexts;

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

    for (const char* text : gAllInterfaceTexts) {
        const char* cursor = text;
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

void createUserInterface(const VulkanContext& ctx, VkRenderPass renderPass, const char* const* caseTexts,
                         int caseTextCount, UserInterface& ui, uint32_t subpass)
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

    // 全部需要字形的文本：公共面板的与 case 自己的合在一起
    gAllInterfaceTexts.clear();
    gAllInterfaceTexts.reserve(COMMON_INTERFACE_TEXT_COUNT + caseTextCount);
    for (int i = 0; i < COMMON_INTERFACE_TEXT_COUNT; ++i) {
        gAllInterfaceTexts.push_back(COMMON_INTERFACE_TEXTS[i]);
    }
    for (int i = 0; i < caseTextCount; ++i) {
        gAllInterfaceTexts.push_back(caseTexts[i]);
    }

    // 默认字体没有汉字，使用系统自带的微软雅黑，字形范围由界面文本本身决定
    ImFontGlyphRangesBuilder rangesBuilder;
    rangesBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (const char* text : gAllInterfaceTexts) {
        rangesBuilder.AddText(text);
    }
    rangesBuilder.BuildRanges(&gGlyphRanges);

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    ImFont* font = nullptr;
#ifdef __ANDROID__
    // 安卓没有系统字体文件路径这一说，字体随包走 assets，和模型贴图一样从内存加载。
    // FontDataOwnedByAtlas 默认是 true，图集会保存传入的指针并在销毁时释放它，
    // 本地这份 vector 不能交给它，改成让图集复制一份
    const std::vector<unsigned char> fontBytes = readAssetBytes("fonts/msyh.ttc");
    fontConfig.FontDataOwnedByAtlas = false;
    font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(fontBytes.data()),
                                          static_cast<int>(fontBytes.size()), 18.0f, &fontConfig,
                                          gGlyphRanges.Data);
#else
    font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18.0f, &fontConfig, gGlyphRanges.Data);
#endif
    if (font == nullptr) {
        FATAL("failed to load the interface font");
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);

#ifndef __ANDROID__
    if (!ImGui_ImplGlfw_InitForVulkan(ctx.window, true)) {
        FATAL("failed to initialize the imgui GLFW backend");
    }
#else
    if (!ImGui_ImplAndroid_Init(ctx.window)) {
        FATAL("failed to initialize the imgui android backend");
    }
#endif

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = VK_API_VERSION_1_2;
    initInfo.Instance = ctx.instance;
    initInfo.PhysicalDevice = ctx.physicalDevice;
    initInfo.Device = ctx.device;
    initInfo.QueueFamily = ctx.queueFamilyIndex;
    initInfo.Queue = ctx.queue;
    initInfo.DescriptorPool = ui.descriptorPool;
    initInfo.RenderPass = renderPass;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = ctx.swapchainImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Subpass = subpass;
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
#ifndef __ANDROID__
    ImGui_ImplGlfw_Shutdown();
#else
    ImGui_ImplAndroid_Shutdown();
#endif
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(ctx.device, ui.descriptorPool, nullptr);
    ui.descriptorPool = VK_NULL_HANDLE;
}

void beginUserInterfaceFrame()
{
    ImGui_ImplVulkan_NewFrame();
#ifndef __ANDROID__
    ImGui_ImplGlfw_NewFrame();
#else
    ImGui_ImplAndroid_NewFrame();
#endif
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

void buildGpuClockPanel(GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SeparatorText(TEXT_SECTION_GPU_LOCK);
    if (!gpuClockLockState.detected) {
        ImGui::TextWrapped(TEXT_GPU_NOT_DETECTED, gpuClockLockState.lastActionDetail.c_str());
        return;
    }

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

void buildTimingPanel(const TimingStore& timing)
{
    ImGui::SeparatorText(TEXT_SECTION_TIMING);
    ImGui::TextWrapped("%s", TEXT_TIMING_HINT);
    ImGui::BeginChild("TimingScrollRegion", ImVec2(0.0f, 420.0f), ImGuiChildFlags_Border);
    for (int i = 0; i < timing.itemCount; ++i) {
        const TimingWindow& window = timing.window[i];
        const char* name = timing.items[i].displayName;

        if (i == 0) {
            // 第一项固定是帧时间，额外显示由它换算出的帧率
            const double fps = window.mean > 0.0 ? 1000.0 / window.mean : 0.0;
            ImGui::Text("%s   %7.3f ± %6.3f ms   (%.0f FPS)", name, window.mean, window.standardDeviation, fps);
        } else {
            ImGui::Text("%s   %7.3f ± %6.3f ms", name, window.mean, window.standardDeviation);
        }

        plotRecentSeries(name, timing.historyTimeSeconds, timing.historyValues[i]);
    }
    ImGui::EndChild();
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
