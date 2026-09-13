#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 泛光与亮部闪烁";

static const char* const TEXT_SECTION_BLOOM = "泛光";
static const char* const TEXT_THRESHOLD = "亮部取出";
static const char* const TEXT_CHAIN = "降采样链";
static const char* const TEXT_LEVELS = "层级数";
static const char* const TEXT_THRESHOLD_VALUE = "阈值";
static const char* const TEXT_INTENSITY = "泛光强度";

static const char* const TEXT_SECTION_ORDER = "后处理顺序";
static const char* const TEXT_ORDER = "抗锯齿与色调映射";

static const char* const THRESHOLD_LABELS[] = { "不做泛光", "硬阈值", "软阈值" };
static const char* const CHAIN_LABELS[] = { "逐级高斯", "一降一升", "一次采样多层级" };
static const char* const ORDER_LABELS[] = { "先抗锯齿", "先色调映射" };

enum { THRESHOLD_LABEL_COUNT = sizeof(THRESHOLD_LABELS) / sizeof(THRESHOLD_LABELS[0]) };
enum { CHAIN_LABEL_COUNT = sizeof(CHAIN_LABELS) / sizeof(CHAIN_LABELS[0]) };
enum { ORDER_LABEL_COUNT = sizeof(ORDER_LABELS) / sizeof(ORDER_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_WORKLOAD_HINT =
    "泛光的开销集中在降采样与上采样的全屏拷贝，以及每一次切换渲染目标带来的通道边界。"
    "层级数每加一级就多两趟全屏拷贝。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "泛光把亮部取出、逐级降采样、再逐级放大叠加，全程应当在线性高动态范围空间里做。硬阈值直接截断，"
    "光源移动时阈值附近的像素在每一帧之间忽进忽出，降采样把这些跳变扩散成整片闪烁；软阈值用一段过渡"
    "把跨越阈值的变化压平，闪烁随之减弱。抗锯齿排在色调映射之前时，平滑作用在高的动态范围上，光源的"
    "高光不会在平滑之前被压扁；排在之后时高光已经被压到显示范围，再平滑就补不回来了。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_BLOOM,    TEXT_THRESHOLD,
    TEXT_CHAIN,           TEXT_LEVELS,           TEXT_THRESHOLD_VALUE,
    TEXT_INTENSITY,       TEXT_SECTION_ORDER,    TEXT_ORDER,
    THRESHOLD_LABELS[0],  THRESHOLD_LABELS[1],   THRESHOLD_LABELS[2],
    CHAIN_LABELS[0],      CHAIN_LABELS[1],       CHAIN_LABELS[2],
    ORDER_LABELS[0],      ORDER_LABELS[1],       TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,   TEXT_RENDER_PASSES,    TEXT_WORKLOAD_HINT,
    TEXT_SECTION_GUIDE,   TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,
    TEXT_EXPLANATION,
};

enum { CASE_INTERFACE_TEXT_COUNT = sizeof(CASE_INTERFACE_TEXTS) / sizeof(CASE_INTERFACE_TEXTS[0]) };

const char* const* caseInterfaceTexts(int& outCount)
{
    static std::vector<const char*> texts;
    if (texts.empty()) {
        for (int i = 0; i < CASE_INTERFACE_TEXT_COUNT; ++i) {
            texts.push_back(CASE_INTERFACE_TEXTS[i]);
        }
        const TimingItemDescription* items = caseTimingItems();
        for (int i = 0; i < TIMING_ID_COUNT; ++i) {
            texts.push_back(items[i].displayName);
        }
    }
    outCount = static_cast<int>(texts.size());
    return texts.data();
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_BLOOM);
    int threshold = static_cast<int>(state.threshold);
    if (ImGui::Combo(TEXT_THRESHOLD, &threshold, THRESHOLD_LABELS, THRESHOLD_LABEL_COUNT)) {
        state.threshold = static_cast<uint32_t>(threshold);
    }
    int chain = static_cast<int>(state.chain);
    if (ImGui::Combo(TEXT_CHAIN, &chain, CHAIN_LABELS, CHAIN_LABEL_COUNT)) {
        state.chain = static_cast<uint32_t>(chain);
    }
    int levels = static_cast<int>(state.levels);
    if (ImGui::SliderInt(TEXT_LEVELS, &levels, 1, 5)) {
        state.levels = static_cast<uint32_t>(levels);
    }
    ImGui::SliderFloat(TEXT_THRESHOLD_VALUE, &state.thresholdValue, 0.5f, 6.0f, "%.2f");
    ImGui::SliderFloat(TEXT_INTENSITY, &state.intensity, 0.0f, 1.5f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_ORDER);
    int order = static_cast<int>(state.order);
    if (ImGui::Combo(TEXT_ORDER, &order, ORDER_LABELS, ORDER_LABEL_COUNT)) {
        state.order = static_cast<uint32_t>(order);
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_RENDER_PASSES, statistics.renderPassCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
