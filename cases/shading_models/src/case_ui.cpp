#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 着色频率与镜面模型";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_SEGMENTS = "细分段数";
static const char* const TEXT_LIGHT_INTENSITY = "平行光强度";
static const char* const TEXT_AMBIENT = "环境项强度";

static const char* const TEXT_SECTION_SHADING = "着色";
static const char* const TEXT_FREQUENCY = "着色频率";
static const char* const TEXT_SPECULAR = "镜面模型";
static const char* const TEXT_NORMAL_SOURCE = "法线来源";
static const char* const TEXT_SHININESS = "高光指数";

static const char* const FREQUENCY_LABELS[] = { "平面着色", "Gouraud", "Phong" };
enum { FREQUENCY_LABEL_COUNT = sizeof(FREQUENCY_LABELS) / sizeof(FREQUENCY_LABELS[0]) };

static const char* const SPECULAR_LABELS[] = { "Blinn-Phong", "Phong" };
enum { SPECULAR_LABEL_COUNT = sizeof(SPECULAR_LABELS) / sizeof(SPECULAR_LABELS[0]) };

static const char* const NORMAL_SOURCE_LABELS[] = { "几何法线", "程序化凹凸" };
enum { NORMAL_SOURCE_LABEL_COUNT = sizeof(NORMAL_SOURCE_LABELS) / sizeof(NORMAL_SOURCE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_TRIANGLES = "三角形 %u 个";
static const char* const TEXT_SCENE_HINT =
    "一个解析生成的球，顶点法线是解析法线，所以几何永远是光滑的，只有轮廓随细分段数变化。"
    "着色频率决定光照算在哪一级：平面着色每个三角形算一次，用屏幕空间导数得到面法线；"
    "Gouraud 每个顶点算一次，把颜色插值到像素；Phong 每个像素算一次，法线插值之后才做光照。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "平面着色与 Phong 的差别是法线的粒度，Gouraud 与 Phong 的差别是光照的粒度。Gouraud 把光照"
    "算在顶点上，高光只要比一个三角形小就整块丢失；细分段数越低丢得越多。Blinn-Phong 用半程向量"
    "代替反射方向，同样的高光指数下它的高光更大更亮。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,        TEXT_SECTION_SCENE,      TEXT_MOVE_SPEED,
    TEXT_SEGMENTS,           TEXT_LIGHT_INTENSITY,    TEXT_AMBIENT,
    TEXT_SECTION_SHADING,    TEXT_FREQUENCY,          FREQUENCY_LABELS[0],
    FREQUENCY_LABELS[1],     FREQUENCY_LABELS[2],     TEXT_SPECULAR,
    SPECULAR_LABELS[0],      SPECULAR_LABELS[1],      TEXT_NORMAL_SOURCE,
    NORMAL_SOURCE_LABELS[0], NORMAL_SOURCE_LABELS[1], TEXT_SHININESS,
    TEXT_SECTION_WORKLOAD,   TEXT_DRAW_COMMANDS,      TEXT_TRIANGLES,
    TEXT_SCENE_HINT,         TEXT_SECTION_GUIDE,      TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,         TEXT_GUIDE_QUIT,         TEXT_GUIDE_DRAG,
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
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    int segments = static_cast<int>(state.segments);
    if (ImGui::SliderInt(TEXT_SEGMENTS, &segments, SHADING_MIN_SEGMENTS, SHADING_MAX_SEGMENTS)) {
        state.segments = static_cast<uint32_t>(segments);
    }
    ImGui::SliderFloat(TEXT_LIGHT_INTENSITY, &state.lightIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat(TEXT_AMBIENT, &state.ambient, 0.0f, 0.5f, "%.3f");

    ImGui::SeparatorText(TEXT_SECTION_SHADING);
    int frequency = static_cast<int>(state.shadingFrequency);
    if (ImGui::Combo(TEXT_FREQUENCY, &frequency, FREQUENCY_LABELS, FREQUENCY_LABEL_COUNT)) {
        state.shadingFrequency = static_cast<uint32_t>(frequency);
    }
    int specular = static_cast<int>(state.specularModel);
    if (ImGui::Combo(TEXT_SPECULAR, &specular, SPECULAR_LABELS, SPECULAR_LABEL_COUNT)) {
        state.specularModel = static_cast<uint32_t>(specular);
    }
    // 程序化凹凸是逐像素计算的，平面着色与 Gouraud 用不上它
    const bool bumpAvailable = state.shadingFrequency == SHADING_FREQUENCY_PHONG;
    ImGui::BeginDisabled(!bumpAvailable);
    int normalSource = static_cast<int>(state.normalSource);
    if (ImGui::Combo(TEXT_NORMAL_SOURCE, &normalSource, NORMAL_SOURCE_LABELS, NORMAL_SOURCE_LABEL_COUNT)) {
        state.normalSource = static_cast<uint32_t>(normalSource);
    }
    ImGui::EndDisabled();
    ImGui::SliderFloat(TEXT_SHININESS, &state.shininess, 4.0f, 512.0f, "%.0f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_TRIANGLES, statistics.triangleCount);
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
