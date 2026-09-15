#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 球谐环境光照";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_SKY_INTENSITY = "天空亮度";
static const char* const TEXT_GLOW_INTENSITY = "光晕亮度";
static const char* const TEXT_GLOW_EXPONENT = "光晕锐度";
static const char* const TEXT_ROTATION = "环境旋转角度";
static const char* const TEXT_EXPOSURE = "曝光倍数";

static const char* const TEXT_SECTION_HARMONICS = "球谐";
static const char* const TEXT_BANDS = "阶数";
static const char* const TEXT_BACKGROUND = "背景";
static const char* const TEXT_SHADING = "球的辐照度";
static const char* const TEXT_REFERENCE_SAMPLES = "参考积分采样数";

static const char* const BACKGROUND_LABELS[] = { "球谐重建", "原始环境" };
enum { BACKGROUND_LABEL_COUNT = sizeof(BACKGROUND_LABELS) / sizeof(BACKGROUND_LABELS[0]) };

static const char* const SHADING_LABELS[] = { "球谐系数", "逐像素积分" };
enum { SHADING_LABEL_COUNT = sizeof(SHADING_LABELS) / sizeof(SHADING_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_PROJECTION = "上一次投影用时 %.1f 毫秒";
static const char* const TEXT_SCENE_HINT =
    "环境是一段解析函数：天顶到地平线的渐变、一段平滑过渡落到暗地面，再加一个余弦幂次的光晕。"
    "它在球面上等距采样 256 乘 512 个方向做数值积分，投影成 1 到 5 阶的球谐系数。背景可以直接"
    "显示原始环境，也可以显示用当前阶数重建出来的环境，两者的差就是重建误差。球的漫反射辐照度"
    "要么用系数乘卷积因子得到，要么在片元里逐像素对半球做数值积分作为对照。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "球谐把环境压成一组系数，重建的细节由阶数决定：一阶只有四个分量，只能表达上下左右的大致明暗；"
    "阶数越高越接近原环境，代价是系数变多、重建的乘法也变多。漫反射辐照度还多一步：每一项要乘上"
    "Ramamoorthi 给出的卷积因子，其中三阶的因子恰好是零，所以三阶系数对漫反射没有贡献，把阶数从"
    "三阶提到四阶时球的画面不会变。光晕锐度是余弦的幂次，指数越大光晕越集中，也越难用低阶球谐"
    "表示，重建会在光晕周围出现过冲与暗环。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_SCENE,     TEXT_MOVE_SPEED,
    TEXT_SKY_INTENSITY,     TEXT_GLOW_INTENSITY,    TEXT_GLOW_EXPONENT,
    TEXT_ROTATION,          TEXT_EXPOSURE,          TEXT_SECTION_HARMONICS,
    TEXT_BANDS,             TEXT_BACKGROUND,        BACKGROUND_LABELS[0],
    BACKGROUND_LABELS[1],   TEXT_SHADING,           SHADING_LABELS[0],
    SHADING_LABELS[1],      TEXT_REFERENCE_SAMPLES, TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,     TEXT_PROJECTION,        TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE,     TEXT_GUIDE_MOVE,        TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,        TEXT_GUIDE_DRAG,        TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(600.0f, 940.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat(TEXT_SKY_INTENSITY, &state.skyIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat(TEXT_GLOW_INTENSITY, &state.glowIntensity, 0.0f, 40.0f, "%.2f");
    ImGui::SliderFloat(TEXT_GLOW_EXPONENT, &state.glowExponent, 1.0f, 64.0f, "%.0f");
    ImGui::SliderFloat(TEXT_ROTATION, &state.rotationDegrees, 0.0f, 360.0f, "%.0f");
    ImGui::SliderFloat(TEXT_EXPOSURE, &state.exposure, 0.1f, 8.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_HARMONICS);
    int bands = static_cast<int>(state.bandCount);
    if (ImGui::SliderInt(TEXT_BANDS, &bands, 1, SH_MAX_BANDS)) {
        state.bandCount = static_cast<uint32_t>(bands);
    }
    int background = static_cast<int>(state.background);
    if (ImGui::Combo(TEXT_BACKGROUND, &background, BACKGROUND_LABELS, BACKGROUND_LABEL_COUNT)) {
        state.background = static_cast<uint32_t>(background);
    }
    int shading = static_cast<int>(state.shading);
    if (ImGui::Combo(TEXT_SHADING, &shading, SHADING_LABELS, SHADING_LABEL_COUNT)) {
        state.shading = static_cast<uint32_t>(shading);
    }
    int samples = static_cast<int>(state.referenceSamples);
    if (ImGui::SliderInt(TEXT_REFERENCE_SAMPLES, &samples, 16, SH_REFERENCE_MAX_SAMPLES)) {
        state.referenceSamples = static_cast<uint32_t>(samples);
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_PROJECTION, state.projectionMilliseconds);
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
