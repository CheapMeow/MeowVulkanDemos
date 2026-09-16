#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 程序化噪声";

static const char* const TEXT_SECTION_NOISE = "噪声";
static const char* const TEXT_KIND = "种类";
static const char* const TEXT_FREQUENCY = "频率";
static const char* const TEXT_OCTAVES = "倍频数";
static const char* const TEXT_PERSISTENCE = "持续度";
static const char* const TEXT_LACUNARITY = "间隙度";
static const char* const TEXT_REFERENCE = "参考超采样数";
static const char* const TEXT_DISPLAY = "显示缩放";

static const char* const KIND_LABELS[] = { "值噪声", "Perlin 梯度噪声", "Worley 细胞噪声",
                                           "多倍频叠加", "超采样参考" };
enum { KIND_LABEL_COUNT = sizeof(KIND_LABELS) / sizeof(KIND_LABELS[0]) };

static const char* const TEXT_SECTION_ANIMATION = "动画";
static const char* const TEXT_TIME = "时间";
static const char* const TEXT_ANIMATE = "自动推进";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_EVALUATIONS = "每像素求值 %u 次";
static const char* const TEXT_SCENE_HINT =
    "整幅画面由一段噪声函数直接算出来，没有纹理也没有几何。值噪声在格点上放随机数，格内按曲线"
    "插值；Perlin 的梯度噪声在格点上放随机方向，取值是梯度与相对位置的点积，格点处必然为零，"
    "因此不会出现方格状的方向偏置；Worley 噪声在每个格子里放一个特征点，取到最近特征点的距离，"
    "形状是细胞与边缘。多倍频叠加把同一段噪声按频率乘间隙度、振幅乘持续度叠若干层。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "多倍频叠加是程序化纹理的常用手段：每一层提供一段频率区间，层的振幅按持续度衰减，"
    "得到的结果比单层更接近自然纹理的频谱。代价是每加一层就多一次噪声求值。"
    "频率过高时逐像素求值会欠采样，参考一档按像素覆盖的区域做超采样，"
    "两者的差就是闪烁的来源。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_NOISE,   TEXT_KIND,
    KIND_LABELS[0],        KIND_LABELS[1],       KIND_LABELS[2],
    KIND_LABELS[3],        KIND_LABELS[4],       TEXT_FREQUENCY,
    TEXT_OCTAVES,          TEXT_PERSISTENCE,     TEXT_LACUNARITY,
    TEXT_REFERENCE,        TEXT_DISPLAY,         TEXT_SECTION_ANIMATION,
    TEXT_TIME,             TEXT_ANIMATE,         TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,    TEXT_EVALUATIONS,     TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE,    TEXT_GUIDE_QUIT,      TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(580.0f, 940.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_NOISE);
    int kind = static_cast<int>(state.kind);
    if (ImGui::Combo(TEXT_KIND, &kind, KIND_LABELS, KIND_LABEL_COUNT)) {
        state.kind = static_cast<uint32_t>(kind);
    }
    ImGui::SliderFloat(TEXT_FREQUENCY, &state.frequency, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    int octaves = static_cast<int>(state.octaves);
    if (ImGui::SliderInt(TEXT_OCTAVES, &octaves, 1, 8)) {
        state.octaves = static_cast<uint32_t>(octaves);
    }
    ImGui::SliderFloat(TEXT_PERSISTENCE, &state.persistence, 0.1f, 0.9f, "%.2f");
    ImGui::SliderFloat(TEXT_LACUNARITY, &state.lacunarity, 1.2f, 4.0f, "%.2f");
    int reference = static_cast<int>(state.referenceSide);
    if (ImGui::SliderInt(TEXT_REFERENCE, &reference, 1, 8)) {
        state.referenceSide = static_cast<uint32_t>(reference);
    }
    ImGui::SliderFloat(TEXT_DISPLAY, &state.displayScale, 0.2f, 4.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_ANIMATION);
    ImGui::SliderFloat(TEXT_TIME, &state.timeSeconds, 0.0f, 60.0f, "%.2f");
    ImGui::Checkbox(TEXT_ANIMATE, &state.animate);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_EVALUATIONS, statistics.evaluationCount);
    ImGui::TextWrapped("%s", TEXT_SCENE_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
