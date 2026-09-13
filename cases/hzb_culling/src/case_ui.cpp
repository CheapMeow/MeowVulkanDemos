#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "HZB 遮挡剔除";

static const char* const TEXT_SECTION_CULLING = "遮挡剔除";
static const char* const TEXT_ENABLE_CULLING = "启用遮挡剔除";
static const char* const TEXT_EXTREME = "金字塔取值取向";
static const char* const TEXT_LEVEL_MODE = "层级选择";
static const char* const TEXT_LEVEL = "固定层级";
static const char* const TEXT_DEPTH_SOURCE = "深度来源";
static const char* const TEXT_EXPANSION = "包围盒扩大系数";
static const char* const TEXT_VISUALIZE = "显示金字塔层级";

static const char* const TEXT_SECTION_CAMERA = "相机";
static const char* const TEXT_YAW = "水平角度";

static const char* const EXTREME_LABELS[] = { "最远深度", "最近深度" };
static const char* const LEVEL_MODE_LABELS[] = { "固定层级", "按屏幕投影尺寸" };
static const char* const DEPTH_SOURCE_LABELS[] = { "上一帧深度", "当前帧深度" };

enum { EXTREME_LABEL_COUNT = sizeof(EXTREME_LABELS) / sizeof(EXTREME_LABELS[0]) };
enum { LEVEL_MODE_LABEL_COUNT = sizeof(LEVEL_MODE_LABELS) / sizeof(LEVEL_MODE_LABELS[0]) };
enum { DEPTH_SOURCE_LABEL_COUNT = sizeof(DEPTH_SOURCE_LABELS) / sizeof(DEPTH_SOURCE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_INSTANCE_COUNT = "实例总数 %u 个";
static const char* const TEXT_VISIBLE_COUNT = "可见实例 %u 个";
static const char* const TEXT_CULLED_COUNT = "剔除实例 %u 个";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_WORKLOAD_HINT =
    "剔除在计算着色器里完成：每个实例的包围盒投影到屏幕，取覆盖到的整片金字塔纹素做极值，"
    "再把包围盒的最近深度与这个极值比较。可见实例被写进一个列表，间接绘制命令的实例数由原子加累加出来。"
    "数量取自上一帧，界面上显示的是上一帧统计出来的结果。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "取最远深度时判定是保守的：包围盒的最近点比这一片纹素里最远的场景深度还远，说明整个包围盒确实被挡住，"
    "剔除不会漏掉可见物体。改用最近深度，只要包围盒覆盖到的任意一个纹素上有近处物体，整个包围盒就会被剔掉，"
    "跨在遮挡物边缘上的物体因此整块消失。把包围盒扩大系数调大可以减轻这种误剔，代价是漏剔变多。"
    "层级选择按包围盒的屏幕投影尺寸抬高一级，让一次遮挡测试读的纹素数量与包围盒覆盖的像素数无关。"
    "深度取自上一帧时不需要预通道，代价是多了一帧的延迟，新露出的物体会被错误剔除一帧；"
    "取当前帧时先跑一遍深度预通道，剔除结果与当前画面完全一致。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,        TEXT_SECTION_CULLING,     TEXT_ENABLE_CULLING,
    TEXT_EXTREME,            TEXT_LEVEL_MODE,          TEXT_LEVEL,
    TEXT_DEPTH_SOURCE,       TEXT_EXPANSION,           TEXT_VISUALIZE,
    TEXT_SECTION_CAMERA,     TEXT_YAW,
    EXTREME_LABELS[0],       EXTREME_LABELS[1],
    LEVEL_MODE_LABELS[0],    LEVEL_MODE_LABELS[1],
    DEPTH_SOURCE_LABELS[0],  DEPTH_SOURCE_LABELS[1],
    TEXT_SECTION_WORKLOAD,   TEXT_INSTANCE_COUNT,      TEXT_VISIBLE_COUNT,
    TEXT_CULLED_COUNT,       TEXT_DRAW_COMMANDS,       TEXT_WORKLOAD_HINT,
    TEXT_SECTION_GUIDE,      TEXT_GUIDE_QUIT,          TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(600.0f, 940.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_CULLING);
    ImGui::Checkbox(TEXT_ENABLE_CULLING, &state.occlusionCulling);

    int extreme = static_cast<int>(state.pyramidExtreme);
    if (ImGui::Combo(TEXT_EXTREME, &extreme, EXTREME_LABELS, EXTREME_LABEL_COUNT)) {
        state.pyramidExtreme = static_cast<uint32_t>(extreme);
    }

    int levelMode = static_cast<int>(state.levelMode);
    if (ImGui::Combo(TEXT_LEVEL_MODE, &levelMode, LEVEL_MODE_LABELS, LEVEL_MODE_LABEL_COUNT)) {
        state.levelMode = static_cast<uint32_t>(levelMode);
    }

    int level = static_cast<int>(state.level);
    if (ImGui::SliderInt(TEXT_LEVEL, &level, 0, static_cast<int>(HZB_LEVEL_COUNT) - 1)) {
        state.level = static_cast<uint32_t>(level);
    }

    int depthSource = state.currentFrameDepth ? 1 : 0;
    if (ImGui::Combo(TEXT_DEPTH_SOURCE, &depthSource, DEPTH_SOURCE_LABELS, DEPTH_SOURCE_LABEL_COUNT)) {
        state.currentFrameDepth = depthSource == 1;
    }

    ImGui::SliderFloat(TEXT_EXPANSION, &state.expansion, 1.0f, 1.8f, "%.2f");

    ImGui::Checkbox(TEXT_VISUALIZE, &state.visualize);

    ImGui::SeparatorText(TEXT_SECTION_CAMERA);
    ImGui::SliderFloat(TEXT_YAW, &state.yawDegrees, -60.0f, 60.0f, "%.1f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_INSTANCE_COUNT, statistics.instanceCount);
    ImGui::Text(TEXT_VISIBLE_COUNT, statistics.visibleInstanceCount);
    ImGui::Text(TEXT_CULLED_COUNT, statistics.culledInstanceCount);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
