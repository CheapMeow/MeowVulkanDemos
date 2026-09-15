#include "case_ui.h"

#include "renderer.h"
#include "scene_setup.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 微表面 BRDF";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_LIGHT_INTENSITY = "平行光强度";
static const char* const TEXT_ENVIRONMENT = "均匀环境亮度";
static const char* const TEXT_EXPOSURE = "曝光倍数";

static const char* const TEXT_SECTION_BRDF = "微表面 BRDF";
static const char* const TEXT_LIGHTING = "光照模式";
static const char* const TEXT_DISTRIBUTION = "法线分布";
static const char* const TEXT_GEOMETRY = "几何项";
static const char* const TEXT_FRESNEL = "菲涅耳";
static const char* const TEXT_MULTISCATTER = "多次散射补偿";
static const char* const TEXT_ROUGHNESS = "粗糙度";
static const char* const TEXT_METALLIC = "金属度";
static const char* const TEXT_BASE_COLOR = "基础颜色";
static const char* const TEXT_SAMPLES = "积分采样数";

static const char* const LIGHTING_LABELS[] = { "方向光", "均匀环境", "表格对照" };
enum { LIGHTING_LABEL_COUNT = sizeof(LIGHTING_LABELS) / sizeof(LIGHTING_LABELS[0]) };

static const char* const DISTRIBUTION_LABELS[] = { "GGX", "Beckmann", "Blinn-Phong" };
enum { DISTRIBUTION_LABEL_COUNT = sizeof(DISTRIBUTION_LABELS) / sizeof(DISTRIBUTION_LABELS[0]) };

static const char* const GEOMETRY_LABELS[] = { "Smith", "Schlick", "不做遮蔽" };
enum { GEOMETRY_LABEL_COUNT = sizeof(GEOMETRY_LABELS) / sizeof(GEOMETRY_LABELS[0]) };

static const char* const FRESNEL_LABELS[] = { "Schlick", "常数 F0" };
enum { FRESNEL_LABEL_COUNT = sizeof(FRESNEL_LABELS) / sizeof(FRESNEL_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "一个球加一盏方向光或一份均匀环境。方向光模式下可以看到漫反射、高光与粗糙度的关系；"
    "均匀环境模式是白炉子测试：入射辐射亮度处处相等，出射辐射亮度就等于方向反照率，"
    "球面亮度与背景相等才算能量守恒。单次散射在高粗糙度下会损失能量，补上 Kulla-Conty 的"
    "多次散射项之后才能对上。表格对照模式直接查 CPU 上积分出来的方向反照率表，"
    "用来检查片上积分的那一份有没有写对。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "微表面 BRDF 把表面看成无数微小镜面：法线分布决定高光的宽度，几何项描述微面互相遮挡，"
    "菲涅耳决定反射随角度的变化。三者都只把能量重新分配，不创造能量；但单次散射的模型会漏掉"
    "光线在微面之间来回弹的那部分，粗糙度越高漏得越多，白炉子测试下球面就比背景暗。"
    "Kulla-Conty 用方向反照率把漏掉的部分补回来，补偿量在均匀环境下有闭式解。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_SCENE,    TEXT_MOVE_SPEED,
    TEXT_LIGHT_INTENSITY,   TEXT_ENVIRONMENT,      TEXT_EXPOSURE,
    TEXT_SECTION_BRDF,      TEXT_LIGHTING,         LIGHTING_LABELS[0],
    LIGHTING_LABELS[1],     LIGHTING_LABELS[2],    TEXT_DISTRIBUTION,
    DISTRIBUTION_LABELS[0], DISTRIBUTION_LABELS[1], DISTRIBUTION_LABELS[2],
    TEXT_GEOMETRY,          GEOMETRY_LABELS[0],    GEOMETRY_LABELS[1],
    GEOMETRY_LABELS[2],     TEXT_FRESNEL,          FRESNEL_LABELS[0],
    FRESNEL_LABELS[1],      TEXT_MULTISCATTER,     TEXT_ROUGHNESS,
    TEXT_METALLIC,          TEXT_BASE_COLOR,       TEXT_SAMPLES,
    TEXT_SECTION_WORKLOAD,  TEXT_DRAW_COMMANDS,    TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE,     TEXT_GUIDE_MOVE,       TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,        TEXT_GUIDE_DRAG,       TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(600.0f, 960.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat(TEXT_LIGHT_INTENSITY, &state.lightIntensity, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat(TEXT_ENVIRONMENT, &state.environmentRadiance, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat(TEXT_EXPOSURE, &state.exposure, 0.1f, 8.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_BRDF);
    int lighting = static_cast<int>(state.lighting);
    if (ImGui::Combo(TEXT_LIGHTING, &lighting, LIGHTING_LABELS, LIGHTING_LABEL_COUNT)) {
        state.lighting = static_cast<uint32_t>(lighting);
    }
    int distribution = static_cast<int>(state.distribution);
    if (ImGui::Combo(TEXT_DISTRIBUTION, &distribution, DISTRIBUTION_LABELS, DISTRIBUTION_LABEL_COUNT)) {
        state.distribution = static_cast<uint32_t>(distribution);
    }
    int geometry = static_cast<int>(state.geometry);
    if (ImGui::Combo(TEXT_GEOMETRY, &geometry, GEOMETRY_LABELS, GEOMETRY_LABEL_COUNT)) {
        state.geometry = static_cast<uint32_t>(geometry);
    }
    int fresnel = static_cast<int>(state.fresnel);
    if (ImGui::Combo(TEXT_FRESNEL, &fresnel, FRESNEL_LABELS, FRESNEL_LABEL_COUNT)) {
        state.fresnel = static_cast<uint32_t>(fresnel);
    }
    ImGui::Checkbox(TEXT_MULTISCATTER, &state.multiScattering);
    ImGui::SliderFloat(TEXT_ROUGHNESS, &state.roughness, 0.02f, 1.0f, "%.3f");
    ImGui::SliderFloat(TEXT_METALLIC, &state.metallic, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat(TEXT_BASE_COLOR, &state.baseColor, 0.0f, 1.0f, "%.2f");
    int samples = static_cast<int>(state.furnaceSamples);
    if (ImGui::SliderInt(TEXT_SAMPLES, &samples, 16, 256)) {
        state.furnaceSamples = static_cast<uint32_t>(samples);
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
