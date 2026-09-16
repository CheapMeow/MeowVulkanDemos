#include "case_ui.h"

#include "timing_items.h"
#include "vk_check.h"

#include <imgui.h>

#include <vector>

static const char* const TEXT_PANEL_TITLE = "Vulkan 同步方式的用法对照";

static const char* const TEXT_SECTION_SYNC = "同步方式";
static const char* const TEXT_SYNC_MODE = "生产者与消费者之间";
static const char* const TEXT_SYNC_HINT =
    "下面两行是本方式实际记录进命令缓冲的掩码，与每个像素的图案无关，只决定依赖落在哪一步";

static const char* const TEXT_SECTION_PATTERN = "图案";
static const char* const TEXT_PATTERN_LAYERS = "叠加层数";
static const char* const TEXT_ADVANCE_PHASE = "相位随帧推进";
static const char* const TEXT_PHASE = "相位";

static const char* const SYNC_LABELS[] = {
    "管线屏障", "事件", "二进制信号量", "时间线信号量",
    "围栏", "队列空闲等待", "设备空闲等待", "子通道依赖",
};

static const char* const TEXT_SECTION_WORKLOAD = "本帧工作量";
static const char* const TEXT_DRAW_COMMANDS = "绘制命令 %u 条";
static const char* const TEXT_COMMAND_BUFFERS = "提交的命令缓冲 %u 条";
static const char* const TEXT_TIMELINE_VALUE = "时间线信号量计数值 %llu";
static const char* const TEXT_MEMORY_COHERENT =
    "参数缓冲落在主机一致的内存类型上：主机的写入天然对设备可用，无需冲洗";
static const char* const TEXT_MEMORY_FLUSHED =
    "参数缓冲落在主机可见但非一致的内存类型上：每次写入后都调用 vkFlushMappedMemoryRanges，"
    "这一步是规范里的内存域操作";

static const char* const TEXT_SCENE_HINT =
    "整屏画面由三段构成：生产者把程序化图案写进一张图案纹理，消费者读取这张纹理再加上暗角，"
    "界面上的同步方式决定生产者与消费者之间用哪种原语接起来。八种方式下画面必须逐像素相同，"
    "差别只在命令、掩码、主机是否阻塞与耗时。";

static const char* const TEXT_SECTION_GUIDE = "操作指南";
static const char* const TEXT_GUIDE_QUIT = "Esc 退出程序";
static const char* const TEXT_GUIDE_DRAG = "拖动标题栏可以移动本面板";

static const char* const TEXT_EXPLANATION =
    "管线屏障、事件与子通道依赖都在一次提交之内完成依赖，另外五种方式用两次提交，中间隔一次同步。"
    "阶段掩码限制执行依赖的作用范围：依赖只在源阶段与目标阶段之间成立，别的阶段可以照常并行。"
    "访问掩码决定内存那半边：源访问掩码里的写入被置为可用，目标访问掩码里的访问能看到这些值，"
    "写后读与写后写必须带访问掩码，只有读后写可以只靠执行依赖。"
    "图像布局转换只能由管线屏障或渲染通道完成，信号量、围栏与空闲等待都改不了布局，"
    "用它们做跨提交同步时，转布局的屏障仍然要单独记一条。";

static const char* const CASE_INTERFACE_TEXTS[] = {
    TEXT_PANEL_TITLE,     TEXT_SECTION_SYNC,      TEXT_SYNC_MODE,
    TEXT_SYNC_HINT,       TEXT_SECTION_PATTERN,   TEXT_PATTERN_LAYERS,
    TEXT_ADVANCE_PHASE,   TEXT_PHASE,             SYNC_LABELS[0],
    SYNC_LABELS[1],       SYNC_LABELS[2],         SYNC_LABELS[3],
    SYNC_LABELS[4],       SYNC_LABELS[5],         SYNC_LABELS[6],
    SYNC_LABELS[7],       TEXT_SECTION_WORKLOAD,  TEXT_DRAW_COMMANDS,
    TEXT_COMMAND_BUFFERS, TEXT_TIMELINE_VALUE,    TEXT_MEMORY_COHERENT,
    TEXT_MEMORY_FLUSHED,  TEXT_SCENE_HINT,        TEXT_SECTION_GUIDE,
    TEXT_GUIDE_QUIT,      TEXT_GUIDE_DRAG,        TEXT_EXPLANATION,
};

enum { CASE_INTERFACE_TEXT_COUNT = sizeof(CASE_INTERFACE_TEXTS) / sizeof(CASE_INTERFACE_TEXTS[0]) };

const char* const* caseInterfaceTexts(int& outCount)
{
    // 计时项的显示名与各同步方式的掩码说明也属于界面文本，一并交给字形范围
    static std::vector<const char*> texts;
    if (texts.empty()) {
        for (int i = 0; i < CASE_INTERFACE_TEXT_COUNT; ++i) {
            texts.push_back(CASE_INTERFACE_TEXTS[i]);
        }
        const TimingItemDescription* items = caseTimingItems();
        for (int i = 0; i < TIMING_ID_COUNT; ++i) {
            texts.push_back(items[i].displayName);
        }
        for (uint32_t mode = 0; mode < SYNC_MODE_COUNT; ++mode) {
            const char* firstLine = nullptr;
            const char* secondLine = nullptr;
            syncModeMaskSummary(mode, firstLine, secondLine);
            texts.push_back(firstLine);
            texts.push_back(secondLine);
        }
    }
    outCount = static_cast<int>(texts.size());
    return texts.data();
}

void ensureUserInterface(const VulkanContext& ctx, SynchronizationRenderer& renderer, UserInterface& ui,
                         uint32_t syncMode, uint32_t& currentRenderPassFamily)
{
    const uint32_t family = syncModeRenderPassFamily(syncMode);
    if (family == currentRenderPassFamily) {
        return;
    }

    if (currentRenderPassFamily != UINT32_MAX) {
        // 界面自己的管线与字体贴图都绑在渲染通道上，换通道要等设备停下来再重建
        VK_CHECK(vkDeviceWaitIdle(ctx.device));
        destroyUserInterface(ctx, ui);
    }

    int caseTextCount = 0;
    const char* const* caseTexts = caseInterfaceTexts(caseTextCount);
    if (family == 1) {
        // 子通道模式下界面画在第二个子通道里，那里才有交换链图像
        createUserInterface(ctx, renderer.subpassRenderPass, caseTexts, caseTextCount, ui, 1);
    } else {
        createUserInterface(ctx, renderer.consumerRenderPass, caseTexts, caseTextCount, ui, 0);
    }
    currentRenderPassFamily = family;
}

void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 900.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(TEXT_PANEL_TITLE);

    ImGui::SeparatorText(TEXT_SECTION_SYNC);
    int syncIndex = static_cast<int>(state.syncMode);
    if (ImGui::Combo(TEXT_SYNC_MODE, &syncIndex, SYNC_LABELS, SYNC_MODE_COUNT)) {
        state.syncMode = static_cast<uint32_t>(syncIndex);
    }

    const char* firstLine = nullptr;
    const char* secondLine = nullptr;
    syncModeMaskSummary(state.syncMode, firstLine, secondLine);
    ImGui::TextWrapped("%s", TEXT_SYNC_HINT);
    ImGui::TextWrapped("%s", firstLine);
    ImGui::TextWrapped("%s", secondLine);

    ImGui::SeparatorText(TEXT_SECTION_PATTERN);
    ImGui::SliderFloat(TEXT_PATTERN_LAYERS, &state.patternLayerCount, 1.0f, 256.0f, "%.0f 层");
    ImGui::Checkbox(TEXT_ADVANCE_PHASE, &state.advancePhase);
    ImGui::BeginDisabled(state.advancePhase);
    ImGui::SliderFloat(TEXT_PHASE, &state.phase, 0.0f, 1.0f, "%.3f");
    ImGui::EndDisabled();

    buildGpuClockPanel(gpuClockLockState, gpuClockMonitor);
    buildTimingPanel(timing);

    ImGui::SeparatorText(TEXT_SECTION_WORKLOAD);
    ImGui::Text(TEXT_DRAW_COMMANDS, statistics.drawCallCount);
    ImGui::Text(TEXT_COMMAND_BUFFERS, statistics.commandBufferCount);
    ImGui::Text(TEXT_TIMELINE_VALUE, static_cast<unsigned long long>(statistics.timelineValue));
    ImGui::TextWrapped("%s",
                       statistics.uniformBufferCoherent ? TEXT_MEMORY_COHERENT : TEXT_MEMORY_FLUSHED);
    ImGui::TextWrapped("%s", TEXT_SCENE_HINT);

    ImGui::SeparatorText(TEXT_SECTION_GUIDE);
    ImGui::BulletText(TEXT_GUIDE_QUIT);
    ImGui::BulletText(TEXT_GUIDE_DRAG);

    ImGui::Spacing();
    ImGui::TextWrapped("%s", TEXT_EXPLANATION);

    ImGui::End();
}
