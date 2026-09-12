#include "case_ui.h"

#include "renderer.h"
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

static const char* const TEXT_SECTION_SHADOW_MAP = "阴影贴图";
static const char* const TEXT_SHADOW_MAP_SIZE = "分辨率";
static const char* const TEXT_SHADOW_MAP_BITS = "深度值位数";

// 分辨率与位数两列可选值，控件在下标与取值之间换算
static const uint32_t SHADOW_MAP_SIZE_VALUES[] = { 512, 1024, 2048, 4096 };
static const char* const SHADOW_MAP_SIZE_LABELS[] = { "512", "1024", "2048", "4096" };
static const uint32_t SHADOW_MAP_BITS_VALUES[] = { 8, 16, 32 };
static const char* const SHADOW_MAP_BITS_LABELS[] = { "8 位", "16 位", "32 位" };

enum { SHADOW_MAP_SIZE_VALUE_COUNT = sizeof(SHADOW_MAP_SIZE_VALUES) / sizeof(SHADOW_MAP_SIZE_VALUES[0]) };
enum { SHADOW_MAP_BITS_VALUE_COUNT = sizeof(SHADOW_MAP_BITS_VALUES) / sizeof(SHADOW_MAP_BITS_VALUES[0]) };

static int shadowMapSizeIndex(uint32_t size)
{
    for (int i = 0; i < SHADOW_MAP_SIZE_VALUE_COUNT; ++i) {
        if (SHADOW_MAP_SIZE_VALUES[i] == size) {
            return i;
        }
    }
    return 0;
}

static int shadowMapBitsIndex(uint32_t bits)
{
    for (int i = 0; i < SHADOW_MAP_BITS_VALUE_COUNT; ++i) {
        if (SHADOW_MAP_BITS_VALUES[i] == bits) {
            return i;
        }
    }
    return 0;
}

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SHADOW_MAP =
    "当前贴图 %u × %u，每纹素 %u 位深度，纹素大小由分辨率与光源正交投影共同决定";

static const char* const TEXT_SECTION_ARTIFACTS = "阴影瑕疵处理";
static const char* const TEXT_BACK_FACE_DEPTH = "只写背面深度";
static const char* const TEXT_NORMAL_LIFT = "法线抬升采样点";
static const char* const TEXT_SLOPE_BIAS = "掠射角放大偏移";
static const char* const TEXT_DEPTH_OFFSET = "基础深度偏移";
static const char* const TEXT_GROUND_CASTER = "地面写入阴影贴图";
static const char* const TEXT_ARTIFACT_HINT =
    "三项可以逐项关闭，基础深度偏移可以拉到零。只关掉其中某一项时另外几项仍在兜底，变化不大；"
    "把只写背面深度关掉、再把基础深度偏移拉到零，自阴影条纹会立刻爬满物体表面。把基础深度偏移"
    "调得过大，物体与地面相接处的阴影会脱开，出现漏光。";
static const char* const TEXT_GROUND_HINT =
    "勾上之后地面也写进阴影贴图。地面是一整块没有厚度的平面，它拿自己写入的深度与自己比较，"
    "深度上没有余量，受光比例会按贴图纹素跳变，于是整块地面都会出现条纹；把基础深度偏移调大，"
    "条纹随之减弱。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "阴影通道从光源方向把背面深度写进一张贴图，主通道把像素投影到同一张贴图上做深度比较。"
    "关掉阴影后地面与物体的明暗不再被遮挡关系影响，打开 PCF 则改为在 3×3 范围内多次比较，"
    "阴影边缘从硬边变成渐变。降低分辨率会让阴影边界变粗糙，降低深度值位数会让深度比较的"
    "档位变少，阴影边界随之出现台阶。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,          TEXT_SECTION_SCENE,        TEXT_INSTANCE_COUNT,
    TEXT_MOVE_SPEED,           TEXT_SECTION_LIGHT,        TEXT_LIGHT_YAW,
    TEXT_LIGHT_PITCH,          TEXT_SHADOWS,              TEXT_PCF,
    TEXT_SECTION_SHADOW_MAP,   TEXT_SHADOW_MAP_SIZE,      TEXT_SHADOW_MAP_BITS,
    SHADOW_MAP_SIZE_LABELS[0], SHADOW_MAP_SIZE_LABELS[1], SHADOW_MAP_SIZE_LABELS[2],
    SHADOW_MAP_SIZE_LABELS[3], SHADOW_MAP_BITS_LABELS[0], SHADOW_MAP_BITS_LABELS[1],
    SHADOW_MAP_BITS_LABELS[2], TEXT_SECTION_ARTIFACTS,     TEXT_BACK_FACE_DEPTH,
    TEXT_NORMAL_LIFT,          TEXT_SLOPE_BIAS,           TEXT_DEPTH_OFFSET,
    TEXT_GROUND_CASTER,        TEXT_ARTIFACT_HINT,        TEXT_GROUND_HINT,
    TEXT_SECTION_WORKLOAD,     TEXT_DRAW_COMMANDS,        TEXT_SHADOW_MAP,
    TEXT_SECTION_GUIDE,        TEXT_GUIDE_MOVE,           TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,           TEXT_GUIDE_DRAG,           TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(500.0f, 940.0f), ImGuiCond_FirstUseEver);
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

    ImGui::SeparatorText(TEXT_SECTION_SHADOW_MAP);
    int sizeIndex = shadowMapSizeIndex(state.shadowMapSize);
    if (ImGui::Combo(TEXT_SHADOW_MAP_SIZE, &sizeIndex, SHADOW_MAP_SIZE_LABELS, SHADOW_MAP_SIZE_VALUE_COUNT)) {
        state.shadowMapSize = SHADOW_MAP_SIZE_VALUES[sizeIndex];
    }
    int bitsIndex = shadowMapBitsIndex(state.shadowMapBits);
    if (ImGui::Combo(TEXT_SHADOW_MAP_BITS, &bitsIndex, SHADOW_MAP_BITS_LABELS, SHADOW_MAP_BITS_VALUE_COUNT)) {
        state.shadowMapBits = SHADOW_MAP_BITS_VALUES[bitsIndex];
    }

    ImGui::SeparatorText(TEXT_SECTION_ARTIFACTS);
    ImGui::Checkbox(TEXT_BACK_FACE_DEPTH, &state.shadowBackFaceDepth);
    ImGui::Checkbox(TEXT_NORMAL_LIFT, &state.shadowNormalLift);
    ImGui::Checkbox(TEXT_SLOPE_BIAS, &state.shadowSlopeBias);
    ImGui::SliderFloat(TEXT_DEPTH_OFFSET, &state.shadowDepthOffset, 0.0f, SHADOW_DEPTH_OFFSET_MAX, "%.4f");
    ImGui::TextWrapped(TEXT_ARTIFACT_HINT);

    ImGui::Spacing();
    ImGui::Checkbox(TEXT_GROUND_CASTER, &state.shadowGroundCaster);
    ImGui::TextWrapped(TEXT_GROUND_HINT);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped(TEXT_SHADOW_MAP, state.shadowMapSize, state.shadowMapSize, state.shadowMapBits);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
