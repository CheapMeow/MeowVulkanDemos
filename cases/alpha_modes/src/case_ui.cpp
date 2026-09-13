#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan alpha 模式对照";

static const char* const TEXT_SECTION_MODE = "镂空的处理方式";
static const char* const TEXT_MODE_SELECT = "方式";
static const char* const TEXT_THRESHOLD = "alpha 阈值";

static const char* const TEXT_SECTION_SAMPLING = "采样";
static const char* const TEXT_SAMPLE_COUNT = "采样数";
static const char* const TEXT_COVERAGE_HINT = "alpha to coverage 需要采样数大于一，当前按 alpha test 处理";

static const char* const TEXT_SECTION_ANIMATION = "动画与背景";
static const char* const TEXT_ROTATION_SPEED = "旋转速度";
static const char* const TEXT_BACKGROUND = "背景亮度";

static const char* const MODE_LABELS[] = { "alpha test", "alpha blend", "alpha to coverage" };
static const char* const SAMPLE_LABELS[] = { "1x", "2x", "4x", "8x" };

enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };
enum { SAMPLE_LABEL_COUNT = sizeof(SAMPLE_LABELS) / sizeof(SAMPLE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_FRAGMENT_COUNT = "片元调用 %u 次";
static const char* const TEXT_FRAGMENT_HINT =
    "片元调用次数由片元着色器里的原子累加统计，帧末经缓冲回读。它反映的是真正进入着色器的片元数，"
    "被深度测试提前剔除的片元不会计数。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "alpha test 用 discard 丢掉低于阈值的片元，边界是硬的，放大后能看到锯齿。alpha blend 保留 alpha "
    "交给固定功能混合，边缘过渡最平滑，代价是不写深度、必须由远到近排序，被遮挡的片元也无法提前剔除。"
    "alpha to coverage 用片元输出的 alpha 对已通过覆盖与深度测试的采样点再做一次概率判定，它需要多重"
    "采样，把硬边变成采样点密度决定的过渡带，既不用排序也不用混合。三种方式在同一个旋转角度下抓帧，"
    "比较边界过渡带的宽度与灰度层次；改到覆盖模式并调高采样数，过渡带会更细。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_MODE,     TEXT_MODE_SELECT,
    TEXT_THRESHOLD,        TEXT_SECTION_SAMPLING, TEXT_SAMPLE_COUNT,
    TEXT_COVERAGE_HINT,    TEXT_SECTION_ANIMATION, TEXT_ROTATION_SPEED,
    TEXT_BACKGROUND,       MODE_LABELS[0],        MODE_LABELS[1],
    MODE_LABELS[2],        SAMPLE_LABELS[0],      SAMPLE_LABELS[1],
    SAMPLE_LABELS[2],      SAMPLE_LABELS[3],      TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,    TEXT_FRAGMENT_COUNT,   TEXT_FRAGMENT_HINT,
    TEXT_SECTION_GUIDE,    TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(540.0f, 880.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_MODE);
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo(TEXT_MODE_SELECT, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.mode = static_cast<uint32_t>(mode);
    }
    ImGui::SliderFloat(TEXT_THRESHOLD, &state.threshold, 0.0f, 1.0f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_SAMPLING);
    const uint32_t* sampleOptions = alphaSampleOptions();
    int sampleIndex = 0;
    for (int i = 0; i < ALPHA_SAMPLE_OPTION_COUNT; ++i) {
        if (sampleOptions[i] == state.sampleCount) {
            sampleIndex = i;
        }
    }
    if (ImGui::Combo(TEXT_SAMPLE_COUNT, &sampleIndex, SAMPLE_LABELS, SAMPLE_LABEL_COUNT)) {
        state.sampleCount = sampleOptions[sampleIndex];
    }
    if (state.mode == ALPHA_MODE_TO_COVERAGE && state.sampleCount == 1) {
        ImGui::TextWrapped("%s", TEXT_COVERAGE_HINT);
    }

    ImGui::SeparatorText(TEXT_SECTION_ANIMATION);
    ImGui::SliderFloat(TEXT_ROTATION_SPEED, &state.rotationSpeed, 0.0f, 2.0f, "%.2f 弧度/秒");
    ImGui::SliderFloat(TEXT_BACKGROUND, &state.backgroundIntensity, 0.0f, 1.0f, "%.2f");

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
