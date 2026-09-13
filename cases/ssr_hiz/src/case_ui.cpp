#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "屏幕空间反射与层次遍历";

static const char* const TEXT_ENABLE_REFLECTION = "启用反射";
static const char* const TEXT_SECTION_MARCH = "步进";
static const char* const TEXT_MARCH_MODE = "步进方式";
static const char* const TEXT_MAX_STEPS = "步数上限";
static const char* const TEXT_STEP_LENGTH = "视空间步长";
static const char* const TEXT_STEP_PIXELS = "屏幕像素步长";
static const char* const TEXT_THICKNESS = "厚度阈值";
static const char* const TEXT_MAX_DISTANCE = "推进距离";
static const char* const TEXT_BINARY_REFINE = "命中后二分细化";

static const char* const MARCH_LABELS[] = { "视空间等距", "屏幕像素", "层次遍历" };
enum { MARCH_LABEL_COUNT = sizeof(MARCH_LABELS) / sizeof(MARCH_LABELS[0]) };

static const char* const TEXT_SECTION_ROUGHNESS = "粗糙度与累积";
static const char* const TEXT_TEMPORAL = "时域累积";
static const char* const TEXT_JITTER = "粗糙度抖动";
static const char* const TEXT_JITTER_STRENGTH = "抖动强度";
static const char* const TEXT_EDGE_FADE = "屏幕边缘淡出";

static const char* const TEXT_SECTION_PYRAMID = "金字塔";
static const char* const TEXT_AVERAGE_PYRAMID = "层次遍历用平均金字塔";

static const char* const TEXT_SECTION_VIEW = "显示";
static const char* const TEXT_VISUALIZATION = "可视化";
static const char* const TEXT_VISUALIZE_LEVEL = "可视化层级";
static const char* const TEXT_YAW = "水平角度";

static const char* const VISUALIZATION_LABELS[] = { "关闭", "并排显示两种金字塔", "步进次数热力图" };
enum { VISUALIZATION_LABEL_COUNT = sizeof(VISUALIZATION_LABELS) / sizeof(VISUALIZATION_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_INSTANCE_COUNT = "实例总数 %u 个";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "屏幕空间反射的全部输入只有当前帧的颜色、法线与深度，也就是相机看到的那一层。"
    "射线在屏幕空间步进，走出画面或者走到相机后面就再也没有信息，命中点附近的采样也只能落在这一层上，"
    "画面边缘缺失的反射与物体背后的断裂都来自这里。"
    "视空间等距步进每一步长度固定，屏幕像素步进在屏幕直线上按像素推进，把深度在归一化设备坐标的 z 上线性"
    "插值：z 在屏幕空间本身就是线性的，这条性质让按像素推进得到的深度与真实射线完全一致。"
    "步长越大越省步数，细物体越容易漏掉；二分细化只把命中点收敛得更准，不改变是否命中；"
    "抖动加时域累积用多帧的平均换更高的有效采样率。层次遍历从最粗一级开始，每一级先整块判断射线是否整段"
    "都在这一块的最前面，可以跳过就跳过，不可以就下降一级，跳过时解出与格子边界的精确交点并越过一点，"
    "避免停在边界上反复升降。取最小值的那条金字塔可以支撑这个断言，取平均的那条没有这个性质，"
    "整块跳过会漏掉交点。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_ENABLE_REFLECTION,   TEXT_PANEL_TITLE,        TEXT_SECTION_MARCH,
    TEXT_MARCH_MODE,
    TEXT_MAX_STEPS,           TEXT_STEP_LENGTH,        TEXT_STEP_PIXELS,
    TEXT_THICKNESS,           TEXT_MAX_DISTANCE,       TEXT_BINARY_REFINE,      MARCH_LABELS[0],
    MARCH_LABELS[1],          MARCH_LABELS[2],         TEXT_SECTION_ROUGHNESS,
    TEXT_TEMPORAL,            TEXT_JITTER,             TEXT_JITTER_STRENGTH,
    TEXT_EDGE_FADE,           TEXT_SECTION_PYRAMID,    TEXT_AVERAGE_PYRAMID,
    TEXT_SECTION_VIEW,        TEXT_VISUALIZATION,      TEXT_VISUALIZE_LEVEL,
    TEXT_YAW,                 VISUALIZATION_LABELS[0], VISUALIZATION_LABELS[1],
    VISUALIZATION_LABELS[2],  TEXT_SECTION_WORKLOAD,   TEXT_INSTANCE_COUNT,
    TEXT_DRAW_COMMANDS,       TEXT_SECTION_GUIDE,      TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,          TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(620.0f, 960.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::Checkbox(TEXT_ENABLE_REFLECTION, &state.reflection);

    ImGui::SeparatorText(TEXT_SECTION_MARCH);
    int marchMode = static_cast<int>(state.marchMode);
    if (ImGui::Combo(TEXT_MARCH_MODE, &marchMode, MARCH_LABELS, MARCH_LABEL_COUNT)) {
        state.marchMode = static_cast<uint32_t>(marchMode);
    }
    int maxSteps = static_cast<int>(state.maxSteps);
    if (ImGui::SliderInt(TEXT_MAX_STEPS, &maxSteps, 8, 256)) {
        state.maxSteps = static_cast<uint32_t>(maxSteps);
    }
    ImGui::SliderFloat(TEXT_STEP_LENGTH, &state.stepLength, 0.02f, 0.50f, "%.3f");
    ImGui::SliderFloat(TEXT_STEP_PIXELS, &state.stepPixels, 2.0f, 64.0f, "%.1f");
    ImGui::SliderFloat(TEXT_THICKNESS, &state.thickness, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat(TEXT_MAX_DISTANCE, &state.maxDistance, 2.0f, 40.0f, "%.1f");
    ImGui::Checkbox(TEXT_BINARY_REFINE, &state.binaryRefine);

    ImGui::SeparatorText(TEXT_SECTION_ROUGHNESS);
    ImGui::Checkbox(TEXT_TEMPORAL, &state.temporal);
    ImGui::Checkbox(TEXT_JITTER, &state.jitter);
    ImGui::SliderFloat(TEXT_JITTER_STRENGTH, &state.jitterStrength, 0.0f, 0.5f, "%.3f");
    ImGui::SliderFloat(TEXT_EDGE_FADE, &state.edgeFade, 0.0f, 0.30f, "%.3f");

    ImGui::SeparatorText(TEXT_SECTION_PYRAMID);
    ImGui::Checkbox(TEXT_AVERAGE_PYRAMID, &state.averagePyramid);

    ImGui::SeparatorText(TEXT_SECTION_VIEW);
    int visualization = static_cast<int>(state.visualization);
    if (ImGui::Combo(TEXT_VISUALIZATION, &visualization, VISUALIZATION_LABELS,
                     VISUALIZATION_LABEL_COUNT)) {
        state.visualization = static_cast<uint32_t>(visualization);
    }
    int level = static_cast<int>(state.visualizeLevel);
    if (ImGui::SliderInt(TEXT_VISUALIZE_LEVEL, &level, 0, static_cast<int>(PYRAMID_LEVEL_COUNT) - 1)) {
        state.visualizeLevel = static_cast<uint32_t>(level);
    }
    ImGui::SliderFloat(TEXT_YAW, &state.yawDegrees, -60.0f, 60.0f, "%.1f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_INSTANCE_COUNT, statistics.instanceCount);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
