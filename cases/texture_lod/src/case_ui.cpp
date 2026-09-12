#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 多级纹理与各向异性";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_PITCH = "视野俯角";

static const char* const TEXT_SECTION_SAMPLING = "采样";
static const char* const TEXT_MIPMAPPED = "多级纹理";
static const char* const TEXT_ANISOTROPY = "各向异性";
static const char* const TEXT_LOD_BIAS = "LOD 偏置";
static const char* const TEXT_VIEW_MODE = "视图";

static const char* const ANISOTROPY_LABELS[] = { "1x", "2x", "4x", "8x", "16x" };
static const float ANISOTROPY_VALUES[] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
static const char* const VIEW_LABELS[] = { "正常", "层级对比", "采样足迹" };

enum { ANISOTROPY_LABEL_COUNT = sizeof(ANISOTROPY_LABELS) / sizeof(ANISOTROPY_LABELS[0]) };
enum { VIEW_LABEL_COUNT = sizeof(VIEW_LABELS) / sizeof(VIEW_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "程序生成的高频细节贴图，棋盘格叠细噪声，自带完整的多级纹理。俯角压得越小，地面越接近掠射，"
    "纹理坐标的屏幕导数在一个方向上被拉得很长，采样足迹从方形变成细长的椭圆。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "层级由纹理坐标对屏幕坐标的导数决定：rho 取两个方向导数模长的较大值，层级取 log2(rho)。"
    "关掉多级纹理后最大层级被钳到零，远处只剩一层高频图案，摩尔纹与闪烁随之出现。各向异性按"
    "采样足迹的长短轴比例在长轴方向上多取几次，把掠射处的模糊收窄。层级对比视图把着色器按导数"
    "算出的层级与硬件 textureQueryLod 给出的层级左右并排，两者相差的只有各向异性修正项。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_SCENE,   TEXT_MOVE_SPEED,
    TEXT_PITCH,           TEXT_SECTION_SAMPLING, TEXT_MIPMAPPED,
    TEXT_ANISOTROPY,      TEXT_LOD_BIAS,        TEXT_VIEW_MODE,
    ANISOTROPY_LABELS[0], ANISOTROPY_LABELS[1], ANISOTROPY_LABELS[2],
    ANISOTROPY_LABELS[3], ANISOTROPY_LABELS[4], VIEW_LABELS[0],
    VIEW_LABELS[1],       VIEW_LABELS[2],       TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,   TEXT_SCENE_HINT,      TEXT_SECTION_GUIDE,
    TEXT_GUIDE_MOVE,      TEXT_GUIDE_LOOK,      TEXT_GUIDE_QUIT,
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
    ImGui::SetNextWindowSize(ImVec2(500.0f, 820.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 2.0f, 80.0f, "%.0f");
    ImGui::SliderFloat(TEXT_PITCH, &state.cameraPitchDegrees, 8.0f, 75.0f, "%.0f 度");

    ImGui::SeparatorText(TEXT_SECTION_SAMPLING);
    ImGui::Checkbox(TEXT_MIPMAPPED, &state.mipmapped);

    int anisotropyIndex = 0;
    for (int i = 0; i < ANISOTROPY_LABEL_COUNT; ++i) {
        if (ANISOTROPY_VALUES[i] == state.maxAnisotropy) {
            anisotropyIndex = i;
        }
    }
    if (ImGui::Combo(TEXT_ANISOTROPY, &anisotropyIndex, ANISOTROPY_LABELS, ANISOTROPY_LABEL_COUNT)) {
        state.maxAnisotropy = ANISOTROPY_VALUES[anisotropyIndex];
    }
    ImGui::SliderFloat(TEXT_LOD_BIAS, &state.lodBias, -2.0f, 2.0f, "%.2f");

    int viewIndex = static_cast<int>(state.viewMode);
    if (ImGui::Combo(TEXT_VIEW_MODE, &viewIndex, VIEW_LABELS, VIEW_LABEL_COUNT)) {
        state.viewMode = static_cast<uint32_t>(viewIndex);
    }

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
