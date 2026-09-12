#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 级联阴影与高级阴影";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_LIGHT = "光源";
static const char* const TEXT_LIGHT_YAW = "方位角";
static const char* const TEXT_LIGHT_PITCH = "高度角";
static const char* const TEXT_SHADOWS = "启用阴影";
static const char* const TEXT_PCF = "PCF 软阴影";

static const char* const TEXT_SECTION_CASCADE = "级联";
static const char* const TEXT_CASCADE_COUNT = "级联级数";
static const char* const TEXT_CASCADE_BLEND = "级间融合";

static const char* const TEXT_SECTION_PCSS = "遮挡物搜索";
static const char* const TEXT_BLOCKER_RADIUS = "搜索半径";
static const char* const TEXT_PENUMBRA_SCALE = "半影系数";

static const char* const TEXT_SECTION_SHADOW_MAP = "阴影贴图";
static const char* const TEXT_SHADOW_MAP_SIZE = "分辨率";
static const char* const TEXT_VALUE_MODE = "取值方式";
static const char* const TEXT_COORD_MODE = "坐标计算位置";
static const char* const TEXT_VIEW_MODE = "视图";

static const uint32_t SHADOW_MAP_SIZE_VALUES[] = { 512, 1024, 2048, 4096 };
static const char* const SHADOW_MAP_SIZE_LABELS[] = { "512", "1024", "2048", "4096" };
static const char* const CASCADE_COUNT_LABELS[] = { "1", "2", "4" };
static const uint32_t CASCADE_COUNT_VALUES[] = { 1, 2, 4 };
static const char* const VALUE_MODE_LABELS[] = { "深度比较", "矩与切比雪夫" };
static const char* const COORD_MODE_LABELS[] = { "片元里算", "顶点里算" };
static const char* const VIEW_MODE_LABELS[] = { "正常", "级联着色", "受光比例" };

enum { SHADOW_MAP_SIZE_VALUE_COUNT = sizeof(SHADOW_MAP_SIZE_VALUES) / sizeof(SHADOW_MAP_SIZE_VALUES[0]) };
enum { CASCADE_COUNT_VALUE_COUNT = sizeof(CASCADE_COUNT_VALUES) / sizeof(CASCADE_COUNT_VALUES[0]) };
enum { VALUE_MODE_LABEL_COUNT = sizeof(VALUE_MODE_LABELS) / sizeof(VALUE_MODE_LABELS[0]) };
enum { COORD_MODE_LABEL_COUNT = sizeof(COORD_MODE_LABELS) / sizeof(COORD_MODE_LABELS[0]) };
enum { VIEW_MODE_LABEL_COUNT = sizeof(VIEW_MODE_LABELS) / sizeof(VIEW_MODE_LABELS[0]) };

static const char* const TEXT_SECTION_ARTIFACTS = "阴影瑕疵处理";
static const char* const TEXT_NORMAL_LIFT = "法线抬升采样点";
static const char* const TEXT_SLOPE_BIAS = "掠射角放大偏移";
static const char* const TEXT_DEPTH_OFFSET = "基础深度偏移";
static const char* const TEXT_GROUND_CASTER = "地面写入阴影贴图";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "地在网格上摆放若干实例，另有一块棋盘格地面。级联把视锥按距离切成若干段，每段一张贴图，"
    "近处的段覆盖范围小、纹素细，远处的段覆盖范围大、纹素粗。渲染时阴影通道按级数逐级绘制，"
    "绘制命令条数随级数增长。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "级联用级数换近处精度：级数越多，近处的段越窄，同样的贴图分辨率下纹素越细，代价是阴影通道"
    "的绘制命令按级数增长，级与级的接缝还需要融合。级间融合在分界附近同时取相邻两级并插值，"
    "否则接缝处会出现宽度突变。遮挡物搜索先在大范围内找出遮挡物，按平均遮挡深度估计半影大小，"
    "再用这个半径做 PCF，阴影从物体接触处的硬边过渡到远处的柔和边界。矩与切比雪夫方式把深度"
    "的一阶与二阶矩存进贴图，用切比雪夫不等式估算受光比例，代价是遮挡物与接收面靠得很近时"
    "出现漏光。坐标在顶点里算会把光源空间的位置插值给片元，超出贴图范围的那部分容易出错。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,        TEXT_SECTION_SCENE,     TEXT_INSTANCE_COUNT,
    TEXT_MOVE_SPEED,         TEXT_SECTION_LIGHT,     TEXT_LIGHT_YAW,
    TEXT_LIGHT_PITCH,        TEXT_SHADOWS,           TEXT_PCF,
    TEXT_SECTION_CASCADE,    TEXT_CASCADE_COUNT,     TEXT_CASCADE_BLEND,
    TEXT_SECTION_PCSS,       TEXT_BLOCKER_RADIUS,    TEXT_PENUMBRA_SCALE,
    TEXT_SECTION_SHADOW_MAP, TEXT_SHADOW_MAP_SIZE,   TEXT_VALUE_MODE,
    TEXT_COORD_MODE,         TEXT_VIEW_MODE,         SHADOW_MAP_SIZE_LABELS[0],
    SHADOW_MAP_SIZE_LABELS[1], SHADOW_MAP_SIZE_LABELS[2], SHADOW_MAP_SIZE_LABELS[3],
    CASCADE_COUNT_LABELS[0], CASCADE_COUNT_LABELS[1], CASCADE_COUNT_LABELS[2],
    VALUE_MODE_LABELS[0],    VALUE_MODE_LABELS[1],   COORD_MODE_LABELS[0],
    COORD_MODE_LABELS[1],    VIEW_MODE_LABELS[0],    VIEW_MODE_LABELS[1],
    VIEW_MODE_LABELS[2],     TEXT_SECTION_ARTIFACTS, TEXT_NORMAL_LIFT,
    TEXT_SLOPE_BIAS,         TEXT_DEPTH_OFFSET,      TEXT_GROUND_CASTER,
    TEXT_SECTION_WORKLOAD,   TEXT_DRAW_COMMANDS,     TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE,      TEXT_GUIDE_MOVE,        TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,         TEXT_GUIDE_DRAG,        TEXT_EXPLANATION,
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

static int indexOfValue(const uint32_t* values, int count, uint32_t value)
{
    for (int i = 0; i < count; ++i) {
        if (values[i] == value) {
            return i;
        }
    }
    return 0;
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 940.0f), ImGuiCond_FirstUseEver);
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

    ImGui::SeparatorText(TEXT_SECTION_CASCADE);
    int cascadeIndex = indexOfValue(CASCADE_COUNT_VALUES, CASCADE_COUNT_VALUE_COUNT, state.cascadeCount);
    if (ImGui::Combo(TEXT_CASCADE_COUNT, &cascadeIndex, CASCADE_COUNT_LABELS, CASCADE_COUNT_VALUE_COUNT)) {
        state.cascadeCount = CASCADE_COUNT_VALUES[cascadeIndex];
    }
    ImGui::Checkbox(TEXT_CASCADE_BLEND, &state.cascadeBlend);

    ImGui::SeparatorText(TEXT_SECTION_PCSS);
    ImGui::SliderFloat(TEXT_BLOCKER_RADIUS, &state.blockerRadius, 0.0f, 8.0f, "%.1f");
    ImGui::SliderFloat(TEXT_PENUMBRA_SCALE, &state.penumbraScale, 0.0f, 6.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_SHADOW_MAP);
    int sizeIndex = indexOfValue(SHADOW_MAP_SIZE_VALUES, SHADOW_MAP_SIZE_VALUE_COUNT, state.shadowMapSize);
    if (ImGui::Combo(TEXT_SHADOW_MAP_SIZE, &sizeIndex, SHADOW_MAP_SIZE_LABELS, SHADOW_MAP_SIZE_VALUE_COUNT)) {
        state.shadowMapSize = SHADOW_MAP_SIZE_VALUES[sizeIndex];
    }
    int valueIndex = static_cast<int>(state.valueMode);
    if (ImGui::Combo(TEXT_VALUE_MODE, &valueIndex, VALUE_MODE_LABELS, VALUE_MODE_LABEL_COUNT)) {
        state.valueMode = static_cast<uint32_t>(valueIndex);
    }
    int coordIndex = static_cast<int>(state.coordMode);
    if (ImGui::Combo(TEXT_COORD_MODE, &coordIndex, COORD_MODE_LABELS, COORD_MODE_LABEL_COUNT)) {
        state.coordMode = static_cast<uint32_t>(coordIndex);
    }
    int viewIndex = static_cast<int>(state.viewMode);
    if (ImGui::Combo(TEXT_VIEW_MODE, &viewIndex, VIEW_MODE_LABELS, VIEW_MODE_LABEL_COUNT)) {
        state.viewMode = static_cast<uint32_t>(viewIndex);
    }

    ImGui::SeparatorText(TEXT_SECTION_ARTIFACTS);
    ImGui::Checkbox(TEXT_NORMAL_LIFT, &state.shadowNormalLift);
    ImGui::Checkbox(TEXT_SLOPE_BIAS, &state.shadowSlopeBias);
    ImGui::SliderFloat(TEXT_DEPTH_OFFSET, &state.shadowDepthOffset, 0.0f, 0.004f, "%.4f");
    ImGui::Checkbox(TEXT_GROUND_CASTER, &state.shadowGroundCaster);

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
