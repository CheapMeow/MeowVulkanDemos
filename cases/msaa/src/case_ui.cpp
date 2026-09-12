#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 多重采样与自定义解析";

static const char* const TEXT_SECTION_SCENE = "子场景";
static const char* const TEXT_SCENE_SELECT = "场景";
static const char* const TEXT_BACKGROUND = "背景亮度";

static const char* const TEXT_SECTION_SAMPLING = "采样";
static const char* const TEXT_SAMPLE_COUNT = "采样数";
static const char* const TEXT_RESOLVE_MODE = "解析方式";
static const char* const TEXT_ALPHA_TO_COVERAGE = "镂空走 alpha to coverage";
static const char* const TEXT_FRAGMENT_COST = "片元开销";

static const char* const SCENE_LABELS[] = { "全屏三角", "交叠三角与植被", "高动态范围" };
static const char* const SAMPLE_LABELS[] = { "1x", "2x", "4x", "8x" };
static const char* const RESOLVE_LABELS[] = { "硬件盒式", "逐采样点编码求平均" };

enum { SCENE_LABEL_COUNT = sizeof(SCENE_LABELS) / sizeof(SCENE_LABELS[0]) };
enum { SAMPLE_LABEL_COUNT = sizeof(SAMPLE_LABELS) / sizeof(SAMPLE_LABELS[0]) };
enum { RESOLVE_LABEL_COUNT = sizeof(RESOLVE_LABELS) / sizeof(RESOLVE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_FRAGMENT_COUNT = "片元调用 %u 次";
static const char* const TEXT_FRAGMENT_HINT =
    "片元调用次数由片元着色器里的原子累加统计，帧末经缓冲回读。它应当随画面覆盖与形状数量变化，"
    "而不随采样数变化。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "片元的着色按每像素每图元执行一次，不随采样数放大；显存与带宽则随采样数放大。多重采样只在"
    "同一个像素被多个图元共同覆盖的边界处改变结果，全屏三角这种内部处处被单一图元覆盖的画面，"
    "4x 与 1x 的抓帧完全一致。alpha to coverage 用片元输出的 alpha 对已通过覆盖与深度测试的"
    "采样点再做一次概率判定，给 alpha test 的硬边补上过渡，代价是不写深度。高动态范围下先按"
    "线性值求平均再色调映射，边界会显得没有抗锯齿；先把各采样点编码到感知空间求平均、再解码回来，"
    "过渡带才与硬件盒式解析的观感接近。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_SCENE,   TEXT_SCENE_SELECT,
    TEXT_BACKGROUND,       TEXT_SECTION_SAMPLING, TEXT_SAMPLE_COUNT,
    TEXT_RESOLVE_MODE,     TEXT_ALPHA_TO_COVERAGE, TEXT_FRAGMENT_COST,
    SCENE_LABELS[0],       SCENE_LABELS[1],      SCENE_LABELS[2],
    SAMPLE_LABELS[0],      SAMPLE_LABELS[1],     SAMPLE_LABELS[2],
    SAMPLE_LABELS[3],      RESOLVE_LABELS[0],    RESOLVE_LABELS[1],
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,   TEXT_FRAGMENT_COUNT,
    TEXT_FRAGMENT_HINT,    TEXT_SECTION_GUIDE,   TEXT_GUIDE_QUIT,
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
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 860.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    int sceneIndex = static_cast<int>(state.sceneIndex);
    if (ImGui::Combo(TEXT_SCENE_SELECT, &sceneIndex, SCENE_LABELS, SCENE_LABEL_COUNT)) {
        state.sceneIndex = static_cast<uint32_t>(sceneIndex);
    }
    ImGui::SliderFloat(TEXT_BACKGROUND, &state.backgroundIntensity, 0.0f, 8.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_SAMPLING);
    const uint32_t* sampleOptions = msaaSampleOptions();
    int sampleIndex = 0;
    for (int i = 0; i < MSAA_SAMPLE_OPTION_COUNT; ++i) {
        if (sampleOptions[i] == state.sampleCount) {
            sampleIndex = i;
        }
    }
    if (ImGui::Combo(TEXT_SAMPLE_COUNT, &sampleIndex, SAMPLE_LABELS, SAMPLE_LABEL_COUNT)) {
        state.sampleCount = sampleOptions[sampleIndex];
    }
    int resolveIndex = static_cast<int>(state.resolveMode);
    if (ImGui::Combo(TEXT_RESOLVE_MODE, &resolveIndex, RESOLVE_LABELS, RESOLVE_LABEL_COUNT)) {
        state.resolveMode = static_cast<uint32_t>(resolveIndex);
    }
    ImGui::Checkbox(TEXT_ALPHA_TO_COVERAGE, &state.alphaToCoverage);
    ImGui::SliderFloat(TEXT_FRAGMENT_COST, &state.fragmentCost, 1.0f, 256.0f, "%.0f 次");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_FRAGMENT_COUNT, statistics.fragmentCount);
    ImGui::TextWrapped("%s", TEXT_FRAGMENT_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
