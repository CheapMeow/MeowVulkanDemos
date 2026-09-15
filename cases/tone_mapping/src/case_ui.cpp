#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 色调映射";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_LIGHT_INTENSITY = "平行光强度";
static const char* const TEXT_SKY_INTENSITY = "天空亮度";
static const char* const TEXT_SUN_INTENSITY = "太阳辐射亮度";

static const char* const TEXT_SECTION_TONEMAP = "色调映射";
static const char* const TEXT_OPERATOR = "算子";
static const char* const TEXT_CHANNEL = "作用分量";
static const char* const TEXT_ENCODING = "输出编码";
static const char* const TEXT_EXPOSURE = "曝光 (EV)";
static const char* const TEXT_WHITE_POINT = "白点 W";

static const char* const OPERATOR_LABELS[] = { "不做映射", "Reinhard", "扩展 Reinhard", "ACES",
                                               "Hable filmic" };
enum { OPERATOR_LABEL_COUNT = sizeof(OPERATOR_LABELS) / sizeof(OPERATOR_LABELS[0]) };

static const char* const CHANNEL_LABELS[] = { "逐通道", "按亮度" };
enum { CHANNEL_LABEL_COUNT = sizeof(CHANNEL_LABELS) / sizeof(CHANNEL_LABELS[0]) };

static const char* const ENCODING_LABELS[] = { "线性直写", "伽马 2.2", "sRGB 传递函数" };
enum { ENCODING_LABEL_COUNT = sizeof(ENCODING_LABELS) / sizeof(ENCODING_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_SCENE_HINT =
    "天空加太阳与背包模型先画进一张半精度浮点的高动态范围图像，之后整整幅画面一起过色调映射。"
    "屏幕左上角有一条参考条，十四格分别是线性辐射亮度 0、0.0156、0.031 一直到 64，每格是前一格的"
    "两倍，只经过与画面相同的色调映射与输出编码，抓帧后可以直接读取每格的像素值。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "曝光在色调映射之前乘在线性辐射亮度上，单位是 EV；曲线把线性值压进 [0,1] 之后才做输出编码。"
    "逐通道的方式让三个分量各自过曲线，亮而饱和的颜色会被压向白色；按亮度的方式只压缩亮度，颜色"
    "比例保留，代价是结果可能仍然超过 1。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_SCENE,   TEXT_MOVE_SPEED,    TEXT_LIGHT_INTENSITY,
    TEXT_SKY_INTENSITY,   TEXT_SUN_INTENSITY,   TEXT_SECTION_TONEMAP,
    TEXT_OPERATOR,        OPERATOR_LABELS[0],   OPERATOR_LABELS[1], OPERATOR_LABELS[2],
    OPERATOR_LABELS[3],   OPERATOR_LABELS[4],   TEXT_CHANNEL,       CHANNEL_LABELS[0],
    CHANNEL_LABELS[1],    TEXT_ENCODING,        ENCODING_LABELS[0], ENCODING_LABELS[1],
    ENCODING_LABELS[2],   TEXT_EXPOSURE,        TEXT_WHITE_POINT,   TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,   TEXT_RENDER_PASSES,   TEXT_SCENE_HINT,    TEXT_SECTION_GUIDE,
    TEXT_GUIDE_MOVE,      TEXT_GUIDE_LOOK,      TEXT_GUIDE_QUIT,    TEXT_GUIDE_DRAG,
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
    ImGui::SetNextWindowSize(ImVec2(560.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat(TEXT_LIGHT_INTENSITY, &state.lightIntensity, 0.0f, 40.0f, "%.2f");
    ImGui::SliderFloat(TEXT_SKY_INTENSITY, &state.skyIntensity, 0.0f, 20.0f, "%.2f");
    ImGui::SliderFloat(TEXT_SUN_INTENSITY, &state.sunIntensity, 0.0f, 4000.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_TONEMAP);
    int op = static_cast<int>(state.toneMapOperator);
    if (ImGui::Combo(TEXT_OPERATOR, &op, OPERATOR_LABELS, OPERATOR_LABEL_COUNT)) {
        state.toneMapOperator = static_cast<uint32_t>(op);
    }
    int channel = static_cast<int>(state.toneMapChannel);
    if (ImGui::Combo(TEXT_CHANNEL, &channel, CHANNEL_LABELS, CHANNEL_LABEL_COUNT)) {
        state.toneMapChannel = static_cast<uint32_t>(channel);
    }
    int encoding = static_cast<int>(state.outputEncoding);
    if (ImGui::Combo(TEXT_ENCODING, &encoding, ENCODING_LABELS, ENCODING_LABEL_COUNT)) {
        state.outputEncoding = static_cast<uint32_t>(encoding);
    }
    ImGui::SliderFloat(TEXT_EXPOSURE, &state.exposureEv, -6.0f, 6.0f, "%.2f");
    ImGui::SliderFloat(TEXT_WHITE_POINT, &state.whitePoint, 1.0f, 16.0f, "%.2f");

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
