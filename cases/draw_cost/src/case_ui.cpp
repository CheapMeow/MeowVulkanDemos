#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan 绘制命令的固定成本";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_MATERIAL_COUNT = "材质数量";
static const char* const TEXT_MOVE_SPEED = "移动速度";

static const char* const TEXT_SECTION_SUBMIT = "提交";
static const char* const TEXT_ORDER_MODE = "提交顺序";
static const char* const TEXT_REDUNDANT_BIND = "每条命令前重复绑定";
static const char* const TEXT_PASS_SPLIT = "渲染通道段数";

static const char* const ORDER_LABELS[] = { "按材质分组", "两种材质交替", "乱序" };
enum { ORDER_LABEL_COUNT = sizeof(ORDER_LABELS) / sizeof(ORDER_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "一批屏幕上只有几个像素的小方块，每个方块一条绘制命令。方块本身几乎不着色，因此设备时间"
    "随命令条数的变化就是每条命令的固定成本：命令解码、管线与绑定重配置、流水线气泡、"
    "渲染通道边界。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "一条绘制命令的代价里有一部分与画面内容无关，按命令条数累加，命令覆盖的像素越少这部分占比"
    "越高。按材质分组让同一种材质只绑定一次，交替顺序每一条命令都要换一次材质描述符集，"
    "两者的差别就是状态重配置的成本。每条命令前重复绑定同一套描述符集与顶点缓冲会再加一笔固定"
    "开销。把绘制拆成多段渲染通道，每一段边界都是一次真实的搬运与一次流水线清空，段数越多越慢。"
    "界面上的绘制命令条数等于实例数量，便于把设备时间除以条数换算成单条命令的成本。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,   TEXT_SECTION_SCENE,    TEXT_INSTANCE_COUNT,
    TEXT_MATERIAL_COUNT, TEXT_MOVE_SPEED,      TEXT_SECTION_SUBMIT,
    TEXT_ORDER_MODE,    TEXT_REDUNDANT_BIND,   TEXT_PASS_SPLIT,
    ORDER_LABELS[0],    ORDER_LABELS[1],       ORDER_LABELS[2],
    TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS, TEXT_SCENE_HINT,
    TEXT_SECTION_GUIDE, TEXT_GUIDE_MOVE,        TEXT_GUIDE_LOOK,
    TEXT_GUIDE_QUIT,    TEXT_GUIDE_DRAG,       TEXT_EXPLANATION,
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
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 860.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_INSTANCE_COUNT, &state.activeInstanceCount, 1, maxInstanceCount, "%d");
    ImGui::SliderInt(TEXT_MATERIAL_COUNT, &state.materialCount, 1, 8, "%d");
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");

    ImGui::SeparatorText(TEXT_SECTION_SUBMIT);
    int orderIndex = static_cast<int>(state.orderMode);
    if (ImGui::Combo(TEXT_ORDER_MODE, &orderIndex, ORDER_LABELS, ORDER_LABEL_COUNT)) {
        state.orderMode = static_cast<uint32_t>(orderIndex);
    }
    ImGui::Checkbox(TEXT_REDUNDANT_BIND, &state.redundantBind);
    ImGui::SliderInt(TEXT_PASS_SPLIT, &state.passSplitCount, 1, 16, "%d");

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
