#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 基于图像的光照";

static const char* const TEXT_SECTION_MODE = "光照";
static const char* const TEXT_SPLIT_SUM = "使用分离求和";
static const char* const TEXT_PARALLAX = "视差矫正";
static const char* const TEXT_RECOMPUTE = "重新预计算";

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_WORKLOAD_HINT =
    "预计算只在按下重新预计算之后跑一次：环境贴图、辐照度图、预滤波链的六级与查找表各一遍。"
    "关掉视差矫正之后探针按无限远环境作假设，方块移动时反射的位置会跟着漂。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "分离求和把环境光照拆成三块：漫反射分量预计算成一张辐照度图，镜面分量按粗糙度分级预滤波成一组"
    "环境贴图，材质相关的部分预计算成一张查找表。运行时按粗糙度取预滤波贴图的一级，再乘查找表给出的"
    "系数。关掉分离求和就直接对原始环境贴图取样，粗糙度不再起作用，高光永远是环境贴图本身的形状；"
    "打开之后，第一行的金属球随粗糙度从镜面变成雾面，第二行的非金属球保持较弱的反射。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_MODE,    TEXT_SPLIT_SUM,
    TEXT_PARALLAX,        TEXT_RECOMPUTE,       TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,   TEXT_RENDER_PASSES,   TEXT_WORKLOAD_HINT,
    TEXT_SECTION_GUIDE,   TEXT_GUIDE_QUIT,      TEXT_GUIDE_DRAG,
    TEXT_EXPLANATION,
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
    ImGui::SetNextWindowSize(ImVec2(580.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_MODE);
    ImGui::Checkbox(TEXT_SPLIT_SUM, &state.splitSum);
    ImGui::Checkbox(TEXT_PARALLAX, &state.parallaxCorrection);
    if (ImGui::Button(TEXT_RECOMPUTE)) {
        state.recomputeRequested = true;
    }

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_RENDER_PASSES, statistics.renderPassCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
