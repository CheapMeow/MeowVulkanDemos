#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 渲染路径对照";

static const char* const TEXT_SECTION_PATH = "路径";
static const char* const TEXT_PATH_SELECT = "提交与着色";
static const char* const TEXT_LIGHT_COUNT = "光源数量";
static const char* const TEXT_TILE_SIZE = "分块尺寸";
static const char* const TEXT_MAX_PER_TILE = "每块的光源上限";

static const char* const PATH_LABELS[] = { "前向", "延迟", "分块前向" };

enum { PATH_LABEL_COUNT = sizeof(PATH_LABELS) / sizeof(PATH_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_TILE_COUNT = "分块 %u 个";
static const char* const TEXT_WORKLOAD_HINT =
    "前向的片元着色复杂度随光源数线性上升；延迟把光照搬到屏幕空间，复杂度与光源数线性而与物体数无关，"
    "代价是几何缓冲的写入与读取；分块前向先用计算着色器按屏幕分块建光源列表，每个片元只过本块的光源。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "三条路径用同一份几何与同一组光源。把光源数量从几十加到上千，比较三条曲线的斜率：前向的斜率最大，"
    "因为它对每个片元都跑完整的光源循环；延迟的斜率小得多，因为光照只在屏幕上做一遍，与物体数量无关；"
    "分块前向介于两者之间，并且把每个片元的有效光源数压到本块列表的长度以内，光源越多优势越明显。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_PATH,    TEXT_PATH_SELECT,
    TEXT_LIGHT_COUNT,     TEXT_TILE_SIZE,       TEXT_MAX_PER_TILE,
    PATH_LABELS[0],       PATH_LABELS[1],       PATH_LABELS[2],
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,  TEXT_TILE_COUNT,
    TEXT_WORKLOAD_HINT,   TEXT_SECTION_GUIDE,   TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,      TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(580.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_PATH);
    int path = static_cast<int>(state.path);
    if (ImGui::Combo(TEXT_PATH_SELECT, &path, PATH_LABELS, PATH_LABEL_COUNT)) {
        state.path = static_cast<uint32_t>(path);
    }
    int lightCount = static_cast<int>(state.lightCount);
    if (ImGui::SliderInt(TEXT_LIGHT_COUNT, &lightCount, 1, static_cast<int>(MAX_LIGHT_COUNT))) {
        state.lightCount = static_cast<uint32_t>(lightCount);
    }
    int tileSize = static_cast<int>(state.tileSize);
    if (ImGui::SliderInt(TEXT_TILE_SIZE, &tileSize, 8, 128)) {
        state.tileSize = static_cast<uint32_t>(tileSize);
    }
    int maxPerTile = static_cast<int>(state.maxLightsPerTile);
    if (ImGui::SliderInt(TEXT_MAX_PER_TILE, &maxPerTile, 1, 64)) {
        state.maxLightsPerTile = static_cast<uint32_t>(maxPerTile);
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_TILE_COUNT, statistics.tileCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
