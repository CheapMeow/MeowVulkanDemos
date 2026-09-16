#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 景深与散景";

static const char* const TEXT_SECTION_LENS = "镜头";
static const char* const TEXT_MODE = "处理方式";
static const char* const TEXT_FOCAL_LENGTH = "焦距";
static const char* const TEXT_FNUMBER = "光圈数";
static const char* const TEXT_FOCUS_DISTANCE = "对焦距离";
static const char* const TEXT_MAX_RADIUS = "弥散圆半径上限";
static const char* const TEXT_SAMPLES = "采样数";
static const char* const TEXT_JITTER = "抖动强度";
static const char* const TEXT_BLADES = "光圈叶片数";

static const char* const MODE_LABELS[] = { "关闭", "普通高斯", "圆盘收集" };
enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_LIGHT_INTENSITY = "平行光强度";
static const char* const TEXT_AMBIENT = "环境项强度";
static const char* const TEXT_EXPOSURE = "曝光倍数";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_SCENE_HINT =
    "一块地面加五排按深度排布的小亮球，另有一个前景大球与一个背景大球。深度附件同时当纹理采样，"
    "景深通道用它还原每个像素的视空间距离，再按薄透镜公式算弥散圆。关闭一档相当于针孔相机，"
    "所有深度都清楚；普通高斯只看中心像素的弥散圆，会把远处的亮球糊到对焦平面上；圆盘收集还看"
    "每个采样点的弥散圆，只有够大能覆盖到本像素的样本才被采纳。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "薄透镜模型里，只有正好落在对焦距离上的物点会成一点，其余物点都摊成一个直径与光圈大小、"
    "焦距和对焦距离有关的圆。半径超过一个像素之后画面就开始发虚，光圈越大虚得越快。"
    "渲染时的难点在于信息只有一层：屏幕空间拿不到被前景挡住的背景颜色，所以前景物体边缘的"
    "模糊拉不出它本该盖住的那一片；把弥散圆半径钳到一个上限可以压住这种拉伸。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_LENS,     TEXT_MODE,
    MODE_LABELS[0],        MODE_LABELS[1],        MODE_LABELS[2],
    TEXT_FOCAL_LENGTH,     TEXT_FNUMBER,          TEXT_FOCUS_DISTANCE,
    TEXT_MAX_RADIUS,       TEXT_SAMPLES,          TEXT_JITTER,
    TEXT_BLADES,           TEXT_SECTION_SCENE,    TEXT_MOVE_SPEED,
    TEXT_LIGHT_INTENSITY,  TEXT_AMBIENT,          TEXT_EXPOSURE,
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,    TEXT_RENDER_PASSES,
    TEXT_SCENE_HINT,       TEXT_SECTION_GUIDE,    TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,       TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(600.0f, 980.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_LENS);
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo(TEXT_MODE, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.mode = static_cast<uint32_t>(mode);
    }
    ImGui::SliderFloat(TEXT_FOCAL_LENGTH, &state.focalLength, 0.01f, 0.12f, "%.3f");
    ImGui::SliderFloat(TEXT_FNUMBER, &state.fNumber, 0.7f, 16.0f, "%.1f");
    ImGui::SliderFloat(TEXT_FOCUS_DISTANCE, &state.focusDistance, 0.5f, 10.0f, "%.2f");
    ImGui::SliderFloat(TEXT_MAX_RADIUS, &state.maxRadius, 1.0f, 64.0f, "%.0f");
    int samples = static_cast<int>(state.sampleCount);
    if (ImGui::SliderInt(TEXT_SAMPLES, &samples, 4, 512)) {
        state.sampleCount = static_cast<uint32_t>(samples);
    }
    ImGui::SliderFloat(TEXT_JITTER, &state.jitter, 0.0f, 1.0f, "%.2f");
    int blades = static_cast<int>(state.blades);
    if (ImGui::SliderInt(TEXT_BLADES, &blades, 0, 12)) {
        state.blades = static_cast<uint32_t>(blades);
    }

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 0.2f, 8.0f, "%.2f");
    ImGui::SliderFloat(TEXT_LIGHT_INTENSITY, &state.lightIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat(TEXT_AMBIENT, &state.ambient, 0.0f, 0.5f, "%.3f");
    ImGui::SliderFloat(TEXT_EXPOSURE, &state.exposure, 0.1f, 8.0f, "%.2f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_RENDER_PASSES, statistics.renderPassCount);
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
