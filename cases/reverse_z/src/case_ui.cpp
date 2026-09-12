#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan Reverse-Z 深度精度";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_OBJECT_COUNT = "物体数量";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_DEPTH = "深度";
static const char* const TEXT_REVERSE_Z = "启用 Reverse-Z";
static const char* const TEXT_NEAR_PLANE = "近裁剪面";
static const char* const TEXT_FAR_PLANE = "远裁剪面";
static const char* const TEXT_DEPTH_FORMAT = "深度附件格式";
static const char* const TEXT_VIEW_MODE = "视图";

static const char* const DEPTH_FORMAT_LABELS[] = { "D16_UNORM", "D24_UNORM_S8", "D32_SFLOAT" };
static const char* const VIEW_LABELS[] = { "正常", "深度可视化" };
enum { DEPTH_FORMAT_LABEL_COUNT = sizeof(DEPTH_FORMAT_LABELS) / sizeof(DEPTH_FORMAT_LABELS[0]) };
enum { VIEW_LABEL_COUNT = sizeof(VIEW_LABELS) / sizeof(VIEW_LABELS[0]) };

static const char* const TEXT_SECTION_FIGHTING = "Z-fighting";
static const char* const TEXT_GROUND_OFFSET = "地面高度间距";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_DEPTH_STATE = "深度模式 %s，深度附件格式 %s";
static const char* const TEXT_DEPTH_STANDARD = "标准（近平面 0，远平面 1，判定小于）";
static const char* const TEXT_DEPTH_REVERSED = "Reverse-Z（近平面 1，远平面 0，判定大于）";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_SCENE_HINT =
    "地面由形状完全相同的上下两层组成，上层整体抬高一个很小的间距，两层只有配色不同。"
    "相机贴近地面朝远处看，画面下方的距离上两层的高度差还在深度精度之内，上层稳定地盖住下层；"
    "越往远处透视投影把可用的深度值挤得越窄，高度差掉到浮点深度的一个最低位以下，两层"
    "在同一个深度值上打平，下层的配色就会成片地透出来，这就是 Z-fighting。勾选 Reverse-Z 之后，"
    "投影把近平面映射到 1、远平面映射到 0，浮点的精度分布与场景需要的分布一致，远处的分辨能力"
    "提高几个数量级，下层的配色随即消失。近裁剪面调到更小会让标准深度的远处更差，远裁剪面调到"
    "更大则把远处的深度值挤得更紧，两者都能加重同一个现象。深度可视化按当前附件格式的档数把"
    "窗口深度量化后画成灰度，台阶的疏密就是该格式的分辨能力，定点格式换不换方向都一样。";

static const char* const TEXT_EXPLANATION =
    "投影后的深度是 1/z 的仿射函数，近处精度高、远处精度低。固定点格式的精度沿整个深度范围均匀"
    "分布，改成 D16 或 D24 之后远处的可用档数直接少了几个数量级，Z-fighting 出现得更早；"
    "浮点格式的相邻可表示值之间的间隔与数值本身成正比，换成 Reverse-Z 之后远处的间隔缩小到与近处"
    "相当，同一对表面重新分得开。深度可视化的灰度是窗口深度按当前格式档数量化的结果，可以直观"
    "看到台阶在远处变密。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_SCENE,     TEXT_OBJECT_COUNT,
    TEXT_MOVE_SPEED,        TEXT_SECTION_DEPTH,     TEXT_REVERSE_Z,
    TEXT_NEAR_PLANE,        TEXT_FAR_PLANE,         TEXT_DEPTH_FORMAT,
    TEXT_VIEW_MODE,         DEPTH_FORMAT_LABELS[0], DEPTH_FORMAT_LABELS[1],
    DEPTH_FORMAT_LABELS[2], VIEW_LABELS[0],         VIEW_LABELS[1],
    TEXT_SECTION_FIGHTING,  TEXT_GROUND_OFFSET,     TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,     TEXT_DEPTH_STATE,       TEXT_DEPTH_STANDARD,
    TEXT_DEPTH_REVERSED,    TEXT_SECTION_GUIDE,     TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,        TEXT_GUIDE_QUIT,        TEXT_GUIDE_DRAG,
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
                        int maxObjectCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_OBJECT_COUNT, &state.activeObjectCount, 0, maxObjectCount, "%d");
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 5.0f, 400.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_DEPTH);
    ImGui::Checkbox(TEXT_REVERSE_Z, &state.reverseZ);
    ImGui::SliderFloat(TEXT_NEAR_PLANE, &state.nearPlane, NEAR_PLANE_MIN, NEAR_PLANE_MAX, "%.3f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(TEXT_FAR_PLANE, &state.farPlane, FAR_PLANE_MIN, FAR_PLANE_MAX, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
    int depthFormatIndex = static_cast<int>(state.depthFormatOption);
    if (ImGui::Combo(TEXT_DEPTH_FORMAT, &depthFormatIndex, DEPTH_FORMAT_LABELS, DEPTH_FORMAT_LABEL_COUNT)) {
        state.depthFormatOption = static_cast<uint32_t>(depthFormatIndex);
    }
    int viewIndex = static_cast<int>(state.viewMode);
    if (ImGui::Combo(TEXT_VIEW_MODE, &viewIndex, VIEW_LABELS, VIEW_LABEL_COUNT)) {
        state.viewMode = static_cast<uint32_t>(viewIndex);
    }

    ImGui::SeparatorText(TEXT_SECTION_FIGHTING);
    ImGui::SliderFloat(TEXT_GROUND_OFFSET, &state.groundOffset, GROUND_OFFSET_MIN, GROUND_OFFSET_MAX, "%.3f",
                       ImGuiSliderFlags_Logarithmic);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped(TEXT_DEPTH_STATE, state.reverseZ ? TEXT_DEPTH_REVERSED : TEXT_DEPTH_STANDARD,
                       depthFormatName(state.depthFormatOption));

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_MOVE);
    ImGui::BulletText(TEXT_GUIDE_LOOK);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
