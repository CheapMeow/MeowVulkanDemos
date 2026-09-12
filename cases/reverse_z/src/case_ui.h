#pragma once

#include "gpu_clock_lock.h"
#include "timing.h"

#include <cstdint>

// 界面上可以调整的参数
struct UiState {
    int activeObjectCount;
    float cameraMoveSpeed;
    // 深度比较的方向与裁剪面。改动后渲染器会重建管线并重新生成投影矩阵
    bool reverseZ;
    float nearPlane;
    float farPlane;
    // 上下两层地面的高度间距，决定远处在哪个距离上开始出现 Z-fighting
    float groundOffset;
};

// 界面上显示的本帧工作量，耗时统计由 TimingStore 提供
struct UiStatistics {
    uint32_t drawCallCount;
};

// 本 case 自己的界面文本，交给公共界面层构建字形范围并逐个校验字形
const char* const* caseInterfaceTexts(int& outCount);

// 构建本 case 的控制面板，公共的耗时面板与锁频面板由公共界面层绘制
void buildUserInterface(UiState& state, const UiStatistics& statistics, const TimingStore& timing,
                        int maxObjectCount, GpuClockLockState& gpuClockLockState,
                        GpuClockMonitor& gpuClockMonitor);
