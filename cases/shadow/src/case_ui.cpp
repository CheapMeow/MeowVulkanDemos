#include "case_ui.h"

#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 阴影贴图";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_LIGHT = "光源";
static const char* const TEXT_LIGHT_YAW = "方位角";
static const char* const TEXT_LIGHT_PITCH = "高度角";
static const char* const TEXT_SHADOWS = "启用阴影";
static const char* const TEXT_PCF = "PCF 软阴影";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SHADOW_MAP = "阴影贴图 2048 × 2048，深度偏移与纹素大小由光源正交投影决定";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "阴影通道从光源方向把背面深度写进一张深度贴图，主通道把像素投影到同一张贴图上做深度比较。"
    "关掉阴影后地面与物体的明暗不再被遮挡关系影响，打开 PCF 则改为在 3×3 范围内多次比较，"
    "阴影边缘从硬边变成渐变。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_SCENE,      TEXT_INSTANCE_COUNT, TEXT_MOVE_SPEED,
    TEXT_SECTION_LIGHT,    TEXT_LIGHT_YAW,          TEXT_LIGHT_PITCH,    TEXT_SHADOWS,
    TEXT_PCF,              TEXT_SECTION_WORKLOAD,   TEXT_DRAW_COMMANDS,  TEXT_SHADOW_MAP,
    TEXT_SECTION_GUIDE,    TEXT_GUIDE_MOVE,         TEXT_GUIDE_LOOK,     TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,       TEXT_EXPLANATION,
};

enum { CASE_INTERFACE_TEXT_COUNT = sizeof(CASE_INTERFACE_TEXTS) / sizeof(CASE_INTERFACE_TEXTS[0]) };

const char* const* caseInterfaceTexts(int& outCount)
{
    // 计时项的显示名也属于界面文本，一并交给字形范围
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
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 820.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_INSTANCE_COUNT, &state.activeInstanceCount, 1, maxInstanceCount, "%d",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 5.0f, 400.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_LIGHT);
    ImGui::SliderFloat(TEXT_LIGHT_YAW, &state.lightYawDegrees, 0.0f, 360.0f, "%.0f 度");
    ImGui::SliderFloat(TEXT_LIGHT_PITCH, &state.lightPitchDegrees, 5.0f, 85.0f, "%.0f 度");
    ImGui::Checkbox(TEXT_SHADOWS, &state.shadowsEnabled);
    ImGui::Checkbox(TEXT_PCF, &state.pcfEnabled);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped("%s", TEXT_SHADOW_MAP);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
