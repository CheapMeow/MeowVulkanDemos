#include "case_ui.h"

#include "renderer.h"
#include "scene_setup.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 高阶纹理过滤";

static const char* const TEXT_SECTION_FILTER = "过滤";
static const char* const TEXT_MODE = "重建核";
static const char* const TEXT_SCALE = "缩放";
static const char* const TEXT_REFERENCE = "参考超采样数";
static const char* const TEXT_DISPLAY = "显示缩放";

static const char* const MODE_LABELS[] = { "最近邻",   "双线性",  "三阶 B 样条",
                                           "Catmull-Rom", "Lanczos2", "Lanczos3",
                                           "解析参考" };
enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_TAPS = "每个像素的纹理读取 %u 次";
static const char* const TEXT_SCENE_HINT =
    "纹理是一段频率沿半径线性增长的图案，另有三条细条纹与一排竖直细线。缩放小于 1 时纹理被放大，"
    "能看到重建核的形状与过冲；大于 1 时被缩小，单个像素盖住多个纹素，只看纹素中心会采出摩尔纹。"
    "参考一档不走纹理：放大时按像素中心直接算解析图案，缩小时按每个像素覆盖的区域做超采样，"
    "相当于理想的重建，用它衡量其余几种核的误差。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "纹理放大是一次重建：纹素是离散的样本，屏幕像素落在样本之间，要用一个核把邻域样本加权求和。"
    "双线性只用四个样本、权重是三角形；三阶 B 样条用十六个样本、权重全为正，结果平滑但偏软；"
    "Catmull-Rom 与 Lanczos 的核带负旁瓣，能把细节拉回来，代价是边缘上出现过冲。"
    "缩小时的正确答案是对像素覆盖的区域做积分，超采样就是它的数值近似。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_FILTER,  TEXT_MODE,          MODE_LABELS[0],
    MODE_LABELS[1],       MODE_LABELS[2],       MODE_LABELS[3],     MODE_LABELS[4],
    MODE_LABELS[5],       MODE_LABELS[6],       TEXT_SCALE,         TEXT_REFERENCE,
    TEXT_DISPLAY,         TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS, TEXT_RENDER_PASSES,
    TEXT_TAPS,            TEXT_SCENE_HINT,      TEXT_SECTION_GUIDE, TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,      TEXT_EXPLANATION,
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

    ImGui::SeparatorText(TEXT_SECTION_FILTER);
    int mode = static_cast<int>(state.filter);
    if (ImGui::Combo(TEXT_MODE, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.filter = static_cast<uint32_t>(mode);
    }
    ImGui::SliderFloat(TEXT_SCALE, &state.scale, 0.02f, 24.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    int reference = static_cast<int>(state.referenceSide);
    if (ImGui::SliderInt(TEXT_REFERENCE, &reference, 1, 16)) {
        state.referenceSide = static_cast<uint32_t>(reference);
    }
    ImGui::SliderFloat(TEXT_DISPLAY, &state.displayScale, 0.2f, 4.0f, "%.2f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_RENDER_PASSES, statistics.renderPassCount);
    ImGui::Text(TEXT_TAPS, statistics.tapCount);
    ImGui::TextWrapped("%s", TEXT_SCENE_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
