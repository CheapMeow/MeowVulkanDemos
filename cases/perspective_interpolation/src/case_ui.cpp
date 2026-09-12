#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 透视矫正插值";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_INTERPOLATION = "插值与图案";
static const char* const TEXT_INTERPOLATION_MODE = "插值方式";
static const char* const TEXT_PATTERN = "图案";
static const char* const TEXT_NEAR_PLANE = "近平面距离";

static const char* const INTERPOLATION_LABELS[] = { "透视矫正", "仿射", "差值图" };
static const char* const PATTERN_LABELS[] = { "棋盘格", "UV 网格" };

enum { INTERPOLATION_LABEL_COUNT = sizeof(INTERPOLATION_LABELS) / sizeof(INTERPOLATION_LABELS[0]) };
enum { PATTERN_LABEL_COUNT = sizeof(PATTERN_LABELS) / sizeof(PATTERN_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "一块只有四个顶点的地面，从相机脚边铺到远处，纹理坐标横跨整块地面，同一个图元在屏幕上的深度"
    "跨度极大。透视矫正按硬件默认路径插值，仿射直接把纹理坐标按屏幕坐标线性插值，两种结果在近处"
    "几乎相同，越远差得越多。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "屏幕空间的线性插值作用在已经被透视除法扭曲过的量上，仿射路径因此把近处拉伸、远处压缩。"
    "正确的做法是插值属性除以深度与深度倒数，再在片元里相除，硬件默认的透视矫正就是这么做的。"
    "切到差值图可以直接看到两种插值在哪些像素上给出了不同的结果。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,
    TEXT_SECTION_SCENE,
    TEXT_MOVE_SPEED,
    TEXT_SECTION_INTERPOLATION,
    TEXT_INTERPOLATION_MODE,
    TEXT_PATTERN,
    TEXT_NEAR_PLANE,
    INTERPOLATION_LABELS[0],
    INTERPOLATION_LABELS[1],
    INTERPOLATION_LABELS[2],
    PATTERN_LABELS[0],
    PATTERN_LABELS[1],
    TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,
    TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE,
    TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(500.0f, 780.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 60.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_INTERPOLATION);
    int interpolationIndex = static_cast<int>(state.interpolationMode);
    if (ImGui::Combo(TEXT_INTERPOLATION_MODE, &interpolationIndex, INTERPOLATION_LABELS,
                     INTERPOLATION_LABEL_COUNT)) {
        state.interpolationMode = static_cast<uint32_t>(interpolationIndex);
    }
    int patternIndex = static_cast<int>(state.patternMode);
    if (ImGui::Combo(TEXT_PATTERN, &patternIndex, PATTERN_LABELS, PATTERN_LABEL_COUNT)) {
        state.patternMode = static_cast<uint32_t>(patternIndex);
    }
    ImGui::SliderFloat(TEXT_NEAR_PLANE, &state.nearPlaneDistance, 0.05f, 12.0f, "%.2f");

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
