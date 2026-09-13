#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 着色吞吐微基准";

static const char* const TEXT_SECTION_KERNEL = "内核";
static const char* const TEXT_KERNEL_SELECT = "类型";
static const char* const TEXT_PRECISION_SELECT = "默认精度";
static const char* const TEXT_ITERATIONS = "迭代次数";
static const char* const TEXT_SAMPLES = "采样次数";
static const char* const TEXT_DIVERGENT = "分支发散";
static const char* const TEXT_RANDOM_LOCALITY = "采样局部性差";

static const char* const KERNEL_LABELS[] = { "算力密集", "采样密集", "分支" };
static const char* const PRECISION_LABELS[] = { "高精度", "中精度" };

enum { KERNEL_LABEL_COUNT = sizeof(KERNEL_LABELS) / sizeof(KERNEL_LABELS[0]) };
enum { PRECISION_LABEL_COUNT = sizeof(PRECISION_LABELS) / sizeof(PRECISION_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_WORKGROUP_COUNT = "线程组 %u 个";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_WORKLOAD_HINT =
    "分支发散打开时同一个线程组里相邻线程走不同的分支，两条分支都要执行再按掩码过滤；关闭时整组一起走"
    "同一条分支。采样局部性影响纹理缓存的命中率，与采样次数是两回事。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "算力密集与采样密集是两类不同的受限：前者看设备时间随迭代次数线性上升，后者看设备时间随采样次数"
    "上升，两者的斜率说明单位时间里能完成多少条算术指令与多少次采样。分支发散时两条分支都要执行，把"
    "发散开关来回切换可以量出同一段计算在两种写法下的比值。中精度在桌面显卡上仍然编译成 32 位浮点，"
    "两档的设备时间相同，真正的差别要在支持 16 位浮点的安卓设备上才能量到。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,       TEXT_SECTION_KERNEL,    TEXT_KERNEL_SELECT,
    TEXT_PRECISION_SELECT,  TEXT_ITERATIONS,        TEXT_SAMPLES,
    TEXT_DIVERGENT,         TEXT_RANDOM_LOCALITY,   KERNEL_LABELS[0],       KERNEL_LABELS[1],
    KERNEL_LABELS[2],       PRECISION_LABELS[0],    PRECISION_LABELS[1],
    TEXT_SECTION_WORKLOAD,  TEXT_WORKGROUP_COUNT,   TEXT_DRAW_COMMANDS,
    TEXT_WORKLOAD_HINT,     TEXT_SECTION_GUIDE,     TEXT_GUIDE_QUIT,
    TEXT_GUIDE_DRAG,        TEXT_EXPLANATION,
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

    ImGui::SeparatorText(TEXT_SECTION_KERNEL);
    int kernel = static_cast<int>(state.kernel);
    if (ImGui::Combo(TEXT_KERNEL_SELECT, &kernel, KERNEL_LABELS, KERNEL_LABEL_COUNT)) {
        state.kernel = static_cast<uint32_t>(kernel);
    }
    int precision = static_cast<int>(state.precision);
    if (ImGui::Combo(TEXT_PRECISION_SELECT, &precision, PRECISION_LABELS, PRECISION_LABEL_COUNT)) {
        state.precision = static_cast<uint32_t>(precision);
    }
    int iterations = static_cast<int>(state.iterations);
    if (ImGui::SliderInt(TEXT_ITERATIONS, &iterations, 1, 256)) {
        state.iterations = static_cast<uint32_t>(iterations);
    }
    int samples = static_cast<int>(state.samples);
    if (ImGui::SliderInt(TEXT_SAMPLES, &samples, 1, 64)) {
        state.samples = static_cast<uint32_t>(samples);
    }
    ImGui::Checkbox(TEXT_DIVERGENT, &state.divergent);
    ImGui::Checkbox(TEXT_RANDOM_LOCALITY, &state.randomLocality);

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_WORKGROUP_COUNT, statistics.workgroupCount);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::TextWrapped("%s", TEXT_WORKLOAD_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
