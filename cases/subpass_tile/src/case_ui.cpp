#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 延迟渲染的子通道与片上存储";

static const char* const TEXT_SECTION_PATH = "路径";
static const char* const TEXT_PATH_SELECT = "几何与光照的分段";
static const char* const TEXT_TRANSIENT = "几何附件作为暂时附件";
static const char* const TEXT_PINGPONG = "乒乓次数";
static const char* const TEXT_PINGPONG_STEP = "乒乓亮度步长";

static const char* const PATH_LABELS[] = { "两个独立渲染通道", "一个通道两个子通道" };

enum { PATH_LABEL_COUNT = sizeof(PATH_LABELS) / sizeof(PATH_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_RENDER_PASSES = "渲染通道 %u 个";
static const char* const TEXT_WORKLOAD_HINT =
    "两个独立渲染通道的路径把几何缓冲解析到主存再读回来；子通道路径把几何缓冲留在片上，光照用输入附件"
    "读它，不产生这一来一回。乒乓每加一趟就多一个渲染通道边界与一次采样。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "同一渲染通道里的两个子通道共享片上存储：几何子通道写出的颜色与法线没有被解析到主存，光照子通道"
    "把它们当输入附件直接读。把负载操作与存储操作都设为不关心，才既能避免把主存内容搬回片上，也能避免"
    "把片上内容写回主存；本 case 里几何附件的存储操作就是由是否暂时附件这一档控制的。每一个渲染通道"
    "边界在分块架构上都是一次真实的搬运，乒乓次数每加一趟就会多一次。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_PATH,     TEXT_PATH_SELECT,
    TEXT_TRANSIENT,        TEXT_PINGPONG,         TEXT_PINGPONG_STEP,
    PATH_LABELS[0],        PATH_LABELS[1],        TEXT_SECTION_WORKLOAD,
    TEXT_DRAW_COMMANDS,    TEXT_RENDER_PASSES,    TEXT_WORKLOAD_HINT,
    TEXT_SECTION_GUIDE,    TEXT_GUIDE_QUIT,       TEXT_GUIDE_DRAG,
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

    ImGui::SeparatorText(TEXT_SECTION_PATH);
    int path = static_cast<int>(state.path);
    if (ImGui::Combo(TEXT_PATH_SELECT, &path, PATH_LABELS, PATH_LABEL_COUNT)) {
        state.path = static_cast<uint32_t>(path);
    }
    ImGui::Checkbox(TEXT_TRANSIENT, &state.transientGeometry);
    int pingPong = static_cast<int>(state.pingPongCount);
    if (ImGui::SliderInt(TEXT_PINGPONG, &pingPong, 0, 8)) {
        state.pingPongCount = static_cast<uint32_t>(pingPong);
    }
    ImGui::SliderFloat(TEXT_PINGPONG_STEP, &state.pingPongStep, 0.0f, 0.05f, "%.4f");

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
