#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 细物体的抗锯齿";

static const char* const TEXT_SECTION_MODE = "抗锯齿方式";
static const char* const TEXT_MODE_SELECT = "方式";
static const char* const TEXT_MSAA_HINT = "多重采样 4x 需要设备支持，超过上限时按可用档位处理";

static const char* const TEXT_SECTION_GEOMETRY = "细杆";
static const char* const TEXT_ROD_COUNT = "细杆数量";
static const char* const TEXT_ROD_WIDTH = "细杆宽度";
static const char* const TEXT_PAN_SPEED = "平移速度";
static const char* const TEXT_BACKGROUND = "背景亮度";

static const char* const TEXT_SECTION_FXAA = "FXAA";
static const char* const TEXT_FXAA_STEPS = "搜索步数";
static const char* const TEXT_FXAA_THRESHOLD = "边缘阈值";

static const char* const MODE_LABELS[] = { "无抗锯齿", "多重采样 4x", "FXAA 后处理" };

enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_WORKLOAD_HINT =
    "几何用一个实例化绘制提交，之后是解析与合成各一次。FXAA 的搜索步数只影响片元着色器内部的循环次数，"
    "与细杆数量无关；多重采样的覆盖测试按细杆的边数增长。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "屏幕宽度不足一个像素的细杆在光栅化阶段本身就只能落到零个或一个像素上，次像素的覆盖信息已经丢掉。"
    "多重采样在光栅化时按采样点判定覆盖，细杆即使只覆盖四分之一个像素，解析后也会得到一个灰度，平移时"
    "亮度连续变化。FXAA 只拿到已经光栅化完的最终图像，它能平滑普通边缘的锯齿，对已经消失或已经满亮的"
    "细杆无能为力，平移时亮度在两级之间跳变。把细杆宽度调到一像素以下、平移速度调慢，就能看到两者的区别。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_MODE,     TEXT_MODE_SELECT,
    TEXT_MSAA_HINT,        TEXT_SECTION_GEOMETRY, TEXT_ROD_COUNT,
    TEXT_ROD_WIDTH,        TEXT_PAN_SPEED,        TEXT_BACKGROUND,
    TEXT_SECTION_FXAA,     TEXT_FXAA_STEPS,       TEXT_FXAA_THRESHOLD,
    MODE_LABELS[0],        MODE_LABELS[1],        MODE_LABELS[2],
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,    TEXT_WORKLOAD_HINT,
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
    ImGui::SetNextWindowSize(ImVec2(540.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_MODE);
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo(TEXT_MODE_SELECT, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.mode = static_cast<uint32_t>(mode);
    }
    if (state.mode == ANTIALIAS_MSAA) {
        ImGui::TextWrapped("%s", TEXT_MSAA_HINT);
    }

    ImGui::SeparatorText(TEXT_SECTION_GEOMETRY);
    int rodCount = static_cast<int>(state.rodCount);
    if (ImGui::SliderInt(TEXT_ROD_COUNT, &rodCount, 4, 256)) {
        state.rodCount = static_cast<uint32_t>(rodCount);
    }
    ImGui::SliderFloat(TEXT_ROD_WIDTH, &state.rodWidthPixels, 0.2f, 6.0f, "%.2f 像素");
    ImGui::SliderFloat(TEXT_PAN_SPEED, &state.panSpeed, 0.0f, 120.0f, "%.0f 像素/秒");
    ImGui::SliderFloat(TEXT_BACKGROUND, &state.backgroundIntensity, 0.0f, 0.5f, "%.2f");

    ImGui::SeparatorText(TEXT_SECTION_FXAA);
    int steps = static_cast<int>(state.fxaaSearchSteps);
    if (ImGui::SliderInt(TEXT_FXAA_STEPS, &steps, 1, 16)) {
        state.fxaaSearchSteps = static_cast<uint32_t>(steps);
    }
    ImGui::SliderFloat(TEXT_FXAA_THRESHOLD, &state.fxaaEdgeThreshold, 0.02f, 0.5f, "%.3f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
