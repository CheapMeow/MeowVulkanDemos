#pragma once

#include "gpu_clock_lock.h"
#include "renderer.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    DrawPath drawPath;
    int activeInstanceCount;
    int activeLightCount;
    float farPlane;
    float cameraMoveSpeed;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t visibleInstanceCount;
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxInstanceCount, int maxLightCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor);
