#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    // 抗锯齿方式
    uint32_t mode;
    // 细杆数量与基准宽度
    uint32_t rodCount;
    float rodWidthPixels;
    // 细杆每秒平移的像素数，以及当前的平移偏移
    float panSpeed;
    float panOffsetPixels;
    // 背景亮度
    float backgroundIntensity;
    // FXAA 的搜索步数与边缘阈值
    uint32_t fxaaSearchSteps;
    float fxaaEdgeThreshold;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
