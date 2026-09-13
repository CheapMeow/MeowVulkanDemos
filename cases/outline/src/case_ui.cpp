#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 后处理描边与双 Pass 外扩";

static const char* const TEXT_SECTION_MODE = "描边方式";
static const char* const TEXT_MODE_SELECT = "方式";
static const char* const TEXT_DEPTH_CHANNEL = "用深度通道";
static const char* const TEXT_NORMAL_CHANNEL = "用法线通道";
static const char* const TEXT_THRESHOLD = "边缘阈值";
static const char* const TEXT_LINE_WIDTH = "后处理线宽";
static const char* const TEXT_EXTRUDE = "外扩距离";
static const char* const TEXT_CAMERA = "相机距离";

static const char* const MODE_LABELS[] = { "无描边", "后处理描边", "双 Pass 外扩" };

enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_WORKLOAD_HINT =
    "后处理描边没有额外几何与额外绘制命令，只在输出通道里多读两张附件；双 Pass 外扩要把每一批几何"
    "多画一遍，绘制命令随之增加。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "后处理描边依赖深度与法线的不连续，阈值敏感，纹理内部的弱边缘会被误判成轮廓，深度接近的相邻物体"
    "也分不开。双 Pass 外扩把几何沿法线推出去再反转正面剔除，线宽由外扩距离直接决定，不受深度精度"
    "影响；代价是绘制命令翻倍，屏幕空间的线宽随相机距离变化，对只有单面的薄片与硬边完全失效。把相机"
    "距离拉远可以看到外扩的线变细，而后处理的线宽不变；切到双 Pass 外扩，上方那一排薄片上不会有描边。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_MODE,   TEXT_MODE_SELECT,
    TEXT_DEPTH_CHANNEL,     TEXT_NORMAL_CHANNEL, TEXT_THRESHOLD,
    TEXT_LINE_WIDTH,        TEXT_EXTRUDE,        TEXT_CAMERA,
    MODE_LABELS[0],         MODE_LABELS[1],      MODE_LABELS[2],
    TEXT_SECTION_WORKLOAD,  TEXT_DRAW_COMMANDS,  TEXT_WORKLOAD_HINT,
    TEXT_SECTION_GUIDE,     TEXT_GUIDE_QUIT,     TEXT_GUIDE_DRAG,
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
    ImGui::Checkbox(TEXT_DEPTH_CHANNEL, &state.depthChannel);
    ImGui::Checkbox(TEXT_NORMAL_CHANNEL, &state.normalChannel);
    ImGui::SliderFloat(TEXT_THRESHOLD, &state.threshold, 0.001f, 0.05f, "%.4f");
    ImGui::SliderFloat(TEXT_LINE_WIDTH, &state.lineWidth, 0.5f, 6.0f, "%.1f 像素");
    ImGui::SliderFloat(TEXT_EXTRUDE, &state.extrudeDistance, 0.002f, 0.08f, "%.3f");
    ImGui::SliderFloat(TEXT_CAMERA, &state.cameraDistance, 1.5f, 8.0f, "%.1f");

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
