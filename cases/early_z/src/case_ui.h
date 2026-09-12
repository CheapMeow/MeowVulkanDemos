#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    int activeInstanceCount;
    float cameraMoveSpeed;
    // 绘制顺序、深度预通道、片元着色器里的死代码 discard 与 alpha test
    uint32_t orderMode;
    bool usePrepass;
    bool insertDiscard;
    bool alphaTest;
    // 片元开销的循环次数，放大之后 early-Z 节省的着色开销才明显
    float fragmentCostIterations;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxInstanceCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor);
