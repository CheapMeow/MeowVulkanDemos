#include "case_ui.h"

#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
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

static const char* const TEXT_EXPLANATION =
    "三条路径共用同一份着色器与同一套剔除判据，画面完全一致。"
    "把实例数量或者远裁剪面调大，观察主机剔除与主机记录命令这两项的变化，"
    "设备时间在三条路径上保持一致。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_PATH,      TEXT_PATH_TRADITIONAL,  TEXT_PATH_INSTANCED,
    TEXT_PATH_INDIRECT,     TEXT_SECTION_SCENE,     TEXT_INSTANCE_COUNT,    TEXT_LIGHT_COUNT,
    TEXT_FAR_PLANE,         TEXT_MOVE_SPEED,        TEXT_SECTION_WORKLOAD,  TEXT_TOTAL_INSTANCES,
    TEXT_VISIBLE_INSTANCES, TEXT_DRAW_COMMANDS,     TEXT_SECTION_GUIDE,     TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,        TEXT_GUIDE_SWITCH,      TEXT_GUIDE_QUIT,        TEXT_GUIDE_DRAG,
    TEXT_EXPLANATION,
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

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

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
