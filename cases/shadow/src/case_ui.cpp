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
static const char* const TEXT_SHADOW_MODE = "阴影模式";
static const char* const TEXT_PCF_RADIUS = "PCF 半径";
static const char* const TEXT_PCSS_SEARCH_RADIUS = "遮挡物搜索半径";
static const char* const TEXT_PCSS_LIGHT_RADIUS = "光源半径";
static const char* const TEXT_PCSS_MIN_PENUMBRA = "最小半影";
static const char* const TEXT_PCSS_MAX_PENUMBRA = "最大半影";

// 阴影模式的四个取值，下标与 ShadowMode 一致
static const char* const SHADOW_MODE_LABELS[] = { "关闭", "PCF", "PCSS", "VSSM" };
enum { SHADOW_MODE_LABEL_COUNT = sizeof(SHADOW_MODE_LABELS) / sizeof(SHADOW_MODE_LABELS[0]) };

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
static const char* const TEXT_SHADOW_MAP_MOMENTS =
    "当前贴图 %u × %u，每纹素两个 32 位浮点数，存深度的一阶与二阶矩并带一条金字塔；"
    "纹素大小由分辨率与光源正交投影共同决定";

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

static const char* const TEXT_VSSM_HINT =
    "VSSM 与 PCSS 共用这四个参数：搜索半径是估遮挡物深度时取的区域大小，光源半径与半影上下限"
    "决定过滤区域。矩按区域大小从贴图的金字塔上取，一次区域查询只采一次，过滤区域再大也不加"
    "采样次数。切比雪夫不等式假定区域内的深度接近单峰分布，区域内同时有遮挡物与远处的接受面"
    "时它会把受光比例估得偏高，表现为阴影边缘的漏光。";
static const char* const TEXT_VSSM_GROUND_HINT =
    "VSSM 下地面固定写进阴影贴图：区域矩要求每个纹素都有深度，贴图里若混进没有几何的纹素，"
    "它们的最远深度会把区域均值抬到接收点之前，切比雪夫不等式的前提不再成立。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "阴影通道从光源方向把背面深度写进一张贴图，主通道把像素投影到同一张贴图上做深度比较。"
    "关掉阴影后地面与物体的明暗不再被遮挡关系影响。PCF 在固定半径上多次比较，边缘从硬边变成渐变，"
    "但这个半径不随遮挡物远近变化。PCSS 先用遮挡物搜索估出遮挡物的平均深度，再按接收点到遮挡物的"
    "距离推算半影宽度，遮挡物越远半影越宽，最后在半影范围上做比较，接触处的阴影锐利、离得远的"
    "阴影发散。VSSM 把阴影通道改成写出深度的一阶与二阶矩，遮挡物搜索与半影过滤都换成一次区域采样，"
    "代价不随半影大小增长。降低分辨率会让阴影边界变粗糙，降低深度值位数会让深度比较的档位变少，"
    "阴影边界随之出现台阶；VSSM 的矩固定用 32 位浮点，位数那一项对它不起作用。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,          TEXT_SECTION_SCENE,        TEXT_INSTANCE_COUNT,
    TEXT_MOVE_SPEED,           TEXT_SECTION_LIGHT,        TEXT_LIGHT_YAW,
    TEXT_LIGHT_PITCH,          TEXT_SHADOW_MODE,          TEXT_PCF_RADIUS,
    TEXT_PCSS_SEARCH_RADIUS,   TEXT_PCSS_LIGHT_RADIUS,    TEXT_PCSS_MIN_PENUMBRA,
    TEXT_PCSS_MAX_PENUMBRA,    SHADOW_MODE_LABELS[0],     SHADOW_MODE_LABELS[1],
    SHADOW_MODE_LABELS[2],     SHADOW_MODE_LABELS[3],     TEXT_SECTION_SHADOW_MAP,
    TEXT_SHADOW_MAP_SIZE,      TEXT_SHADOW_MAP_BITS,      SHADOW_MAP_SIZE_LABELS[0],
    SHADOW_MAP_SIZE_LABELS[1], SHADOW_MAP_SIZE_LABELS[2], SHADOW_MAP_SIZE_LABELS[3],
    SHADOW_MAP_BITS_LABELS[0], SHADOW_MAP_BITS_LABELS[1], SHADOW_MAP_BITS_LABELS[2],
    TEXT_SECTION_ARTIFACTS,    TEXT_BACK_FACE_DEPTH,      TEXT_NORMAL_LIFT,
    TEXT_SLOPE_BIAS,           TEXT_DEPTH_OFFSET,         TEXT_GROUND_CASTER,
    TEXT_ARTIFACT_HINT,        TEXT_GROUND_HINT,          TEXT_VSSM_HINT,
    TEXT_VSSM_GROUND_HINT,     TEXT_SECTION_WORKLOAD,     TEXT_DRAW_COMMANDS,
    TEXT_SHADOW_MAP,           TEXT_SHADOW_MAP_MOMENTS,   TEXT_SECTION_GUIDE,
    TEXT_GUIDE_MOVE,           TEXT_GUIDE_LOOK,           TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,           TEXT_EXPLANATION,
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

    int modeIndex = static_cast<int>(state.shadowMode);
    if (ImGui::Combo(TEXT_SHADOW_MODE, &modeIndex, SHADOW_MODE_LABELS, SHADOW_MODE_LABEL_COUNT)) {
        state.shadowMode = static_cast<uint32_t>(modeIndex);
    }
    if (state.shadowMode == SHADOW_MODE_PCF) {
        ImGui::SliderFloat(TEXT_PCF_RADIUS, &state.pcfRadius, 0.0f, SHADOW_PCF_RADIUS_MAX, "%.1f 纹素");
    } else if (state.shadowMode == SHADOW_MODE_PCSS || state.shadowMode == SHADOW_MODE_VSSM) {
        ImGui::SliderFloat(TEXT_PCSS_SEARCH_RADIUS, &state.pcssSearchRadius, 1.0f,
                           SHADOW_PCSS_SEARCH_RADIUS_MAX, "%.1f 纹素");
        ImGui::SliderFloat(TEXT_PCSS_LIGHT_RADIUS, &state.pcssLightRadius,
                           SHADOW_PCSS_LIGHT_RADIUS_MIN, SHADOW_PCSS_LIGHT_RADIUS_MAX, "%.0f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(TEXT_PCSS_MIN_PENUMBRA, &state.pcssMinPenumbra, 0.0f, 8.0f, "%.1f 纹素");
        ImGui::SliderFloat(TEXT_PCSS_MAX_PENUMBRA, &state.pcssMaxPenumbra, 1.0f,
                           SHADOW_PCSS_MAX_PENUMBRA_LIMIT, "%.1f 纹素");
        if (state.shadowMode == SHADOW_MODE_VSSM) {
            ImGui::TextWrapped(TEXT_VSSM_HINT);
        }
    }

    ImGui::SeparatorText(TEXT_SECTION_SHADOW_MAP);
    int sizeIndex = shadowMapSizeIndex(state.shadowMapSize);
    if (ImGui::Combo(TEXT_SHADOW_MAP_SIZE, &sizeIndex, SHADOW_MAP_SIZE_LABELS, SHADOW_MAP_SIZE_VALUE_COUNT)) {
        state.shadowMapSize = SHADOW_MAP_SIZE_VALUES[sizeIndex];
    }
    int bitsIndex = shadowMapBitsIndex(state.shadowMapBits);
    // 矩固定用 32 位浮点保存，位数只作用于深度模式，在方差软阴影下不起作用
    const bool bitsApplies = state.shadowMode != SHADOW_MODE_VSSM;
    if (!bitsApplies) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Combo(TEXT_SHADOW_MAP_BITS, &bitsIndex, SHADOW_MAP_BITS_LABELS, SHADOW_MAP_BITS_VALUE_COUNT)) {
        state.shadowMapBits = SHADOW_MAP_BITS_VALUES[bitsIndex];
    }
    if (!bitsApplies) {
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText(TEXT_SECTION_ARTIFACTS);
    ImGui::Checkbox(TEXT_BACK_FACE_DEPTH, &state.shadowBackFaceDepth);
    ImGui::Checkbox(TEXT_NORMAL_LIFT, &state.shadowNormalLift);
    ImGui::Checkbox(TEXT_SLOPE_BIAS, &state.shadowSlopeBias);
    ImGui::SliderFloat(TEXT_DEPTH_OFFSET, &state.shadowDepthOffset, 0.0f, SHADOW_DEPTH_OFFSET_MAX, "%.4f");
    ImGui::TextWrapped(TEXT_ARTIFACT_HINT);

    ImGui::Spacing();
    // 方差软阴影要求贴图里每个纹素都有深度，地面固定写进去，这一项在 VSSM 下不可改
    const bool groundForced = state.shadowMode == SHADOW_MODE_VSSM;
    if (groundForced) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox(TEXT_GROUND_CASTER, &state.shadowGroundCaster);
    if (groundForced) {
        ImGui::EndDisabled();
        ImGui::TextWrapped(TEXT_VSSM_GROUND_HINT);
    }
    ImGui::TextWrapped(TEXT_GROUND_HINT);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    if (state.shadowMode == SHADOW_MODE_VSSM) {
        ImGui::TextWrapped(TEXT_SHADOW_MAP_MOMENTS, state.shadowMapSize, state.shadowMapSize);
    } else {
        ImGui::TextWrapped(TEXT_SHADOW_MAP, state.shadowMapSize, state.shadowMapSize, state.shadowMapBits);
    }

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
