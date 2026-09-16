#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    // 重建核，取值见 renderer.h 的 FilterMode
    uint32_t filter;
    // 缩放：小于 1 是放大纹理，大于 1 是缩小
    float scale;
    // 参考路径的超采样数，每边开平方个
    uint32_t referenceSide;
    // 显示缩放
    float displayScale;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
    uint32_t renderPassCount;
    // 本帧每个像素的纹理读取次数
    uint32_t tapCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        GpuClockLockState& gpuClockLockState, GpuClockMonitor& gpuClockMonitor);
