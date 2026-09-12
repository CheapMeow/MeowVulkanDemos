#include "case_ui.h"

#include "renderer.h"
#include "timing_items.h"
#include "user_interface.h"

#include <imgui.h>

#include <vector>

// 本 case 界面的全部文本。字形范围与启动校验都以这份集合加公共文本为准，
// 任何一条文本改动都会同时反映到字形范围里，不会出现缺字形显示成问号的情况
static const char* const TEXT_PANEL_TITLE = "Vulkan early-Z 与深度预通道";

static const char* const TEXT_SECTION_SCENE = "场景";
static const char* const TEXT_INSTANCE_COUNT = "实例数量";
static const char* const TEXT_MOVE_SPEED = "移动速度";
static const char* const TEXT_FRAGMENT_COST = "片元开销";

static const char* const TEXT_SECTION_SUBMIT = "提交与着色";
static const char* const TEXT_ORDER_MODE = "绘制顺序";
static const char* const TEXT_PREPASS = "深度预通道";
static const char* const TEXT_INSERT_DISCARD = "插入永不成立的 discard";
static const char* const TEXT_ALPHA_TEST = "镂空走 alpha test";

static const char* const ORDER_LABELS[] = { "前到后", "后到前", "乱序" };
enum { ORDER_LABEL_COUNT = sizeof(ORDER_LABELS) / sizeof(ORDER_LABELS[0]) };

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_SCENE_HINT =
    "一叠相互交叠的四边形沿视线方向排开，其中一片是程序生成掩码的镂空植被。片元着色器里按"
    "「片元开销」做一段可以调长的循环，让着色开销大到足以看出 early-Z 省下了多少。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_MOVE = "W A S D 前后左右移动，Q 下降，E 上升";
static const char* const TEXT_GUIDE_LOOK = "方向键转动视角，按住左 Shift 加速四倍";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "early-Z 在片元着色之前完成深度测试，被遮挡的片元直接跳过着色，因此前到后的顺序最省着色开销，"
    "后到前最费。片元着色器里出现 discard、写出深度或 alpha test 时，很多图形处理器无法在着色前"
    "拿到确定的深度，early-Z 随之失效，前到后的优势缩小。把 alpha test 挪进深度预通道、主通道改用"
    "相等判定，可以让第二次着色重新拿到 early-Z。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,    TEXT_SECTION_SCENE,   TEXT_INSTANCE_COUNT,
    TEXT_MOVE_SPEED,     TEXT_FRAGMENT_COST,   TEXT_SECTION_SUBMIT,
    TEXT_ORDER_MODE,     TEXT_PREPASS,         TEXT_INSERT_DISCARD,
    TEXT_ALPHA_TEST,     ORDER_LABELS[0],      ORDER_LABELS[1],
    ORDER_LABELS[2],     TEXT_SECTION_WORKLOAD, TEXT_DRAW_COMMANDS,
    TEXT_SCENE_HINT,     TEXT_SECTION_GUIDE,   TEXT_GUIDE_MOVE,
    TEXT_GUIDE_LOOK,     TEXT_GUIDE_QUIT,      TEXT_GUIDE_DRAG,
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
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 880.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SCENE);
    ImGui::SliderInt(TEXT_INSTANCE_COUNT, &state.activeInstanceCount, 1, maxInstanceCount, "%d");
    ImGui::SliderFloat(TEXT_MOVE_SPEED, &state.cameraMoveSpeed, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat(TEXT_FRAGMENT_COST, &state.fragmentCostIterations, 1.0f, 256.0f, "%.0f 次");

    ImGui::SeparatorText(TEXT_SECTION_SUBMIT);
    int orderIndex = static_cast<int>(state.orderMode);
    if (ImGui::Combo(TEXT_ORDER_MODE, &orderIndex, ORDER_LABELS, ORDER_LABEL_COUNT)) {
        state.orderMode = static_cast<uint32_t>(orderIndex);
    }
    ImGui::Checkbox(TEXT_PREPASS, &state.usePrepass);
    ImGui::Checkbox(TEXT_INSERT_DISCARD, &state.insertDiscard);
    ImGui::Checkbox(TEXT_ALPHA_TEST, &state.alphaTest);

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
