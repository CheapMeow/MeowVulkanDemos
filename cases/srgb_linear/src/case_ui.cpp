#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan sRGB 与线性空间";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_COLOR = "颜色空间";
static const char* const TEXT_ALBEDO_SRGB = "反照率按 sRGB 解释";
static const char* const TEXT_NORMAL_SRGB = "法线按 sRGB 解释";
static const char* const TEXT_GAMMA_OUTPUT = "输出做伽马编码";
static const char* const TEXT_TONE_MAP = "色调映射";

static const char* const TONE_MAP_LABELS[] = { "无", "Reinhard", "ACES" };
enum { TONE_MAP_LABEL_COUNT = sizeof(TONE_MAP_LABELS) / sizeof(TONE_MAP_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "背包模型加一盏方向光，屏幕左上角另有一条参考条，四个方块分别按线性值 0.5、0.35、0.18、0.05 "
    "绘制，只经过与画面相同的色调映射与输出编码。抓帧后可以直接读取这四块的像素值。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "颜色类贴图按 sRGB 解码，数据类贴图按线性使用，光照与混合全程在线性空间，最后先做色调映射"
    "再编码回 sRGB。把反照率按线性解释，画面整体偏暗；把法线贴图按 sRGB 解释，法线的分量被重新"
    "分布，光照方向随之出错，明暗交界的位置会跑偏。参考条上的数值可以用抓帧对照理论值：线性 0.5 "
    "经伽马编码后约 0.735，也就是 187 左右。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,   TEXT_SECTION_SCENE, TEXT_MOVE_SPEED,
    TEXT_SECTION_COLOR, TEXT_ALBEDO_SRGB,   TEXT_NORMAL_SRGB,
    TEXT_GAMMA_OUTPUT,  TEXT_TONE_MAP,      TONE_MAP_LABELS[0],
    TONE_MAP_LABELS[1], TONE_MAP_LABELS[2], TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS, TEXT_SCENE_HINT,    TEXT_SECTION_GUIDE,
    TEXT_GUIDE_MOVE,    TEXT_GUIDE_LOOK,    TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,    TEXT_EXPLANATION,
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
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 800.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_COLOR);
    ImGui::Checkbox(TEXT_ALBEDO_SRGB, &state.albedoSrgb);
    ImGui::Checkbox(TEXT_NORMAL_SRGB, &state.normalSrgb);
    ImGui::Checkbox(TEXT_GAMMA_OUTPUT, &state.gammaOutput);
    int toneMapIndex = static_cast<int>(state.toneMapMode);
    if (ImGui::Combo(TEXT_TONE_MAP, &toneMapIndex, TONE_MAP_LABELS, TONE_MAP_LABEL_COUNT)) {
        state.toneMapMode = static_cast<uint32_t>(toneMapIndex);
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped("%s", TEXT_SCENE_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
