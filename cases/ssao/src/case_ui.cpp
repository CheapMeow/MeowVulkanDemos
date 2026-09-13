#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 屏幕空间环境光遮蔽";

static const char* const TEXT_SECTION_MODE = "遮蔽";
static const char* const TEXT_MODE_SELECT = "乘到哪一档光照";
static const char* const TEXT_STRENGTH = "遮蔽强度";
static const char* const TEXT_RADIUS = "采样半径";
static const char* const TEXT_KERNEL = "核大小";
static const char* const TEXT_SAMPLES = "采样数";
static const char* const TEXT_NORMAL_WEIGHT = "法线加权";

static const char* const MODE_LABELS[] = { "无遮蔽", "只乘环境光", "乘全部光照" };

enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_WORKLOAD_HINT =
    "遮蔽通道对每个像素在法线半球上取一圈采样点，把每个采样点投影回屏幕再与深度缓冲比较；"
    "设备时间随采样数近似线性上升。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "屏幕空间环境光遮蔽只用当前帧的深度与法线，靠采样点与深度缓冲的比较得到遮蔽量，作用在环境光与"
    "间接光上。被遮挡物体的背面与屏幕外都没有信息：把相机拉近、让物体贴近画面边缘，边缘处的遮蔽会"
    "突然消失。核大小影响采样点分布的范围，半径影响它们离表面的距离；法线加权用两侧法线的夹角给采样"
    "点降权，可以压掉自遮蔽产生的噪点。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_MODE,   TEXT_MODE_SELECT,
    TEXT_STRENGTH,         TEXT_RADIUS,         TEXT_KERNEL,
    TEXT_SAMPLES,          TEXT_NORMAL_WEIGHT,  MODE_LABELS[0],
    MODE_LABELS[1],        MODE_LABELS[2],      TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,    TEXT_WORKLOAD_HINT,  TEXT_SECTION_GUIDE,
    TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,     TEXT_EXPLANATION,
};

enum { CASE_INTERFACE_TEXT_COUNT = sizeof(CASE_INTERFACE_TEXTS) / sizeof(CASE_INTERFACE_TEXTS[0]) };

const char* const* caseInterfaceTexts(int& outCount)
{
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
    ImGui::SliderFloat(TEXT_STRENGTH, &state.strength, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat(TEXT_RADIUS, &state.radius, 0.02f, 1.5f, "%.3f");
    ImGui::SliderFloat(TEXT_KERNEL, &state.kernelSize, 0.05f, 3.0f, "%.2f");
    int samples = static_cast<int>(state.samples);
    if (ImGui::SliderInt(TEXT_SAMPLES, &samples, 1, 64)) {
        state.samples = static_cast<uint32_t>(samples);
    }
    ImGui::Checkbox(TEXT_NORMAL_WEIGHT, &state.normalWeighting);

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
