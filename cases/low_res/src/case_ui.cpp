#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 低分辨率层与引导式上采样";

static const char* const TEXT_SECTION_RESOLUTION = "低分辨率层";
static const char* const TEXT_RATIO = "比例";
static const char* const TEXT_UPSAMPLE = "上采样方式";
static const char* const TEXT_DEPTH_MODE = "深度与法线的获取";
static const char* const TEXT_BILATERAL_STRENGTH = "双边强度";
static const char* const TEXT_GLOW_INTENSITY = "光斑强度";

static const char* const RATIO_LABELS[] = { "全分辨率", "二分之一", "四分之一" };
static const char* const UPSAMPLE_LABELS[] = { "双线性", "深度法线加权" };
static const char* const DEPTH_LABELS[] = { "拷贝为颜色附件", "子通道内写出", "从全分辨率重新取" };

enum { RATIO_LABEL_COUNT = sizeof(RATIO_LABELS) / sizeof(RATIO_LABELS[0]) };
enum { UPSAMPLE_LABEL_COUNT = sizeof(UPSAMPLE_LABELS) / sizeof(UPSAMPLE_LABELS[0]) };
enum { DEPTH_LABEL_COUNT = sizeof(DEPTH_LABELS) / sizeof(DEPTH_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_LOW_RES_SIZE = "低分辨率层 %u x %u";
static const char* const TEXT_WORKLOAD_HINT =
    "低分辨率层的着色与带宽按比例平方下降，上采样在合成通道里按全分辨率执行，开销与比例无关。"
    "深度与法线的三种获取方式代价不同：单独一趟降采样要多读一遍全分辨率的深度与法线，与光斑在同一个"
    "子通道里写出省掉这一趟，从全分辨率重新取则把开销挪到合成通道的每个邻域采样上。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "低频大面积的半透明效果可以按二分之一或四分之一分辨率渲染，再合成回全分辨率，省下填充率与带宽。"
    "直接双线性放大会把低分辨率层的颜色糊到相邻的物体上，在高频几何的边缘产生串色；用全分辨率的深度与"
    "法线给低分辨率邻域加权之后，跨物体的邻域权重接近零，边缘就干净了。关掉深度法线加权并把比例调到"
    "四分之一，把光斑强度调高，就能看到高频物体的边缘被光斑串色。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_RESOLUTION, TEXT_RATIO,
    TEXT_UPSAMPLE,         TEXT_DEPTH_MODE,        TEXT_BILATERAL_STRENGTH,
    TEXT_GLOW_INTENSITY,   RATIO_LABELS[0],        RATIO_LABELS[1],
    RATIO_LABELS[2],       UPSAMPLE_LABELS[0],     UPSAMPLE_LABELS[1],
    DEPTH_LABELS[0],       DEPTH_LABELS[1],        DEPTH_LABELS[2],
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,     TEXT_LOW_RES_SIZE,
    TEXT_WORKLOAD_HINT,    TEXT_SECTION_GUIDE,     TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,       TEXT_EXPLANATION,
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

    ImGui::SeparatorText(TEXT_SECTION_RESOLUTION);
    int ratio = static_cast<int>(state.ratio);
    if (ImGui::Combo(TEXT_RATIO, &ratio, RATIO_LABELS, RATIO_LABEL_COUNT)) {
        state.ratio = static_cast<uint32_t>(ratio);
    }
    int upsample = static_cast<int>(state.upsample);
    if (ImGui::Combo(TEXT_UPSAMPLE, &upsample, UPSAMPLE_LABELS, UPSAMPLE_LABEL_COUNT)) {
        state.upsample = static_cast<uint32_t>(upsample);
    }
    int depthMode = static_cast<int>(state.depthMode);
    if (ImGui::Combo(TEXT_DEPTH_MODE, &depthMode, DEPTH_LABELS, DEPTH_LABEL_COUNT)) {
        state.depthMode = static_cast<uint32_t>(depthMode);
    }
    ImGui::SliderFloat(TEXT_BILATERAL_STRENGTH, &state.bilateralStrength, 1.0f, 400.0f, "%.0f");
    ImGui::SliderFloat(TEXT_GLOW_INTENSITY, &state.glowIntensity, 0.0f, 4.0f, "%.2f");

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_LOW_RES_SIZE, statistics.lowResWidth, statistics.lowResHeight);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
