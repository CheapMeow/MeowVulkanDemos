#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 半透明与顺序无关透明";

static const char* const TEXT_SECTION_BLEND = "混合方式";
static const char* const TEXT_MODE_SELECT = "方式";
static const char* const TEXT_LAYER_COUNT = "层数";
static const char* const TEXT_HEATMAP = "叠加次数热力图";

static const char* const MODE_LABELS[] = { "源混合：由远到近", "源混合：由近到远", "源混合：乱序",
                                           "加权混合", "逐像素链表" };

enum { MODE_LABEL_COUNT = sizeof(MODE_LABELS) / sizeof(MODE_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_NODE_COUNT = "链表节点 %u 个";
static const char* const TEXT_NODE_OVERFLOW = "节点池溢出 %u 个片元";
static const char* const TEXT_LIST_BYTES = "链表缓冲 %.1f 兆字节";
static const char* const TEXT_WORKLOAD_HINT =
    "链表节点数与节点池溢出只在逐像素链表方式下有效，节点池按固定容量一次分配，与层数无关，溢出时多出的"
    "片元被丢弃。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "半透明不写深度，被遮挡的片元无法提前剔除，叠加层数直接决定着色与带宽开销。源混合要求由远到近的"
    "顺序，改到由近到远或乱序，颜色会明显不同。加权混合用一遍累积颜色与权重、一遍累积露出度，不需要"
    "排序，结果与正确顺序接近但有偏差。逐像素链表用原子操作把每个像素上的片元串成链表，再在解析通道里"
    "按深度排序后混合，结果与正确顺序一致，代价是一块与屏幕尺寸和层数相关的显存，以及一遍额外的解析。"
    "把叠加次数热力图打开，画面中心的叠加次数等于层数。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,      TEXT_SECTION_BLEND,   TEXT_MODE_SELECT,
    TEXT_LAYER_COUNT,      TEXT_HEATMAP,         MODE_LABELS[0],
    MODE_LABELS[1],        MODE_LABELS[2],       MODE_LABELS[3],
    MODE_LABELS[4],        TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,
    TEXT_NODE_COUNT,       TEXT_NODE_OVERFLOW,   TEXT_LIST_BYTES,
    TEXT_WORKLOAD_HINT,    TEXT_SECTION_GUIDE,   TEXT_GUIDE_QUIT,
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

    ImGui::SeparatorText(TEXT_SECTION_BLEND);
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo(TEXT_MODE_SELECT, &mode, MODE_LABELS, MODE_LABEL_COUNT)) {
        state.mode = static_cast<uint32_t>(mode);
    }
    int layerCount = static_cast<int>(state.layerCount);
    if (ImGui::SliderInt(TEXT_LAYER_COUNT, &layerCount, 1, TRANSPARENCY_MAX_LAYERS)) {
        state.layerCount = static_cast<uint32_t>(layerCount);
    }
    ImGui::Checkbox(TEXT_HEATMAP, &state.heatmap);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_NODE_COUNT, statistics.nodeCount);
    if (statistics.nodeOverflow > 0) {
        ImGui::Text(TEXT_NODE_OVERFLOW, statistics.nodeOverflow);
    }
    ImGui::Text(TEXT_LIST_BYTES, static_cast<double>(statistics.listBytes) / (1024.0 * 1024.0));
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
