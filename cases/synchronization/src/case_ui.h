#pragma once

#include "gpu_clock_lock.h"
#include "renderer.h"
#include "timing.h"
#include "user_interface.h"
#include "vk_context.h"

#include <cstdint>

struct UiState {
    // 生产者与消费者之间用哪种同步方式接起来
    uint32_t syncMode;
    // 图案的叠加层数，放大生产者的工作量
    float patternLayerCount;
    // 图案相位，关掉"相位随帧推进"之后由滑块固定
    float phase;
    bool advancePhase;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t commandBufferCount;
    uint64_t timelineValue;
    bool uniformBufferCoherent;
    bool uniformFlushPerformed;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 同步方式决定界面记录进哪个渲染通道，方式改变时重建界面。每帧绘制界面之前调用。
// currentRenderPassFamily 由调用方保存，初始值为 UINT32_MAX，安卓端窗口重建后要重新置回它
void ensureUserInterface(const VulkanContext& ctx, SynchronizationRenderer& renderer, UserInterface& ui,
                         uint32_t syncMode, uint32_t& currentRenderPassFamily);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
